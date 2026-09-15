# I/O latency: where a swap page's time actually goes

Status: measurement record and a list of **unimplemented** candidates. Nothing
in §4 or §5 has been built. §2 and §3 are measured results, including two
negative ones that should stop the corresponding work being proposed again.
Baseline: `nbd-vram.c` at `96a6e2a` on `claude/brave-babbage-thjztw`, which is
`develop` plus the `VRAM_PERF=1` instrumentation. Every line number below is
against that revision.

All numbers come from two runs of the same benchmark on the same machine,
captured with `VRAM_PERF=1`. Read §1 for what the benchmark is, because the
distinction between its phases is what makes the rest of the numbers mean
different things.

---

## 1. The benchmark and which half of it matters

One cycle:

1. Fill the device with ~9 GiB of anonymous pages, driven by two consecutive
   forced reclaims:
   `echo "4096M swappiness=max" > /sys/fs/cgroup/memory.reclaim`
2. `swapoff` the device, which makes the kernel read every swapped page back.
3. Bring up a fresh device, `swapon`, and fill it again.

Step 1 is the **write** path. Step 2 is a *read* path but **not the one that
matters**: `swapoff` is used here only as a way to force the kernel to pull
everything back, and its access pattern is not the one a running system
produces. The read path that matters is ordinary demand paging — a process
touching a page that was reclaimed — which appears in the log as the read
traffic during and after step 1.

The two have measurably different shapes, so they are kept apart throughout.

| | normal demand paging | `swapoff` |
|---|---|---|
| sequential reads (`perf/rd-locality`) | **10–21%** | 68–77% |
| requests per batch | 1.12–1.21 | **1.00** |
| worker busy | **0.6–2.1%** | 13.4% |
| read throughput | 5.1–7.0 MiB/s | 116.2 MiB/s |

`vm.page-cluster=0` on this machine, so swap readahead is off and every read
request is exactly one 4 KiB block — `107010 req / 107015 blk` in one window.
That is why the demand-paging rows above are not an artifact of readahead
scattering requests; the kernel really is asking for one page at a time.

---

## 2. Where the time goes

### 2.1 Not VRAM, not PCIe, not the allocator

Lifetime PCIe traffic was 2.5 GiB each way across 282 s — about 18 MiB/s on a
bus that does 10+ GB/s. Compression is doing its job (7.08 GiB written became
2.53 GiB on the wire) and bandwidth is nowhere near a limit.

The single global allocator mutex, which was the first suspect, is not
involved: **0.0% contention across 1,813,294 allocations**, and zero contended
index stripe locks. `ext_scan_words` is 1.02 per allocation, so the bitmap hint
almost always hits on the first word. There is no lock work worth doing.

### 2.2 The cost is per-page round trip, and half the threads are asleep

`perf/worker` in both runs shows **two of four workers handling literally zero
requests for the entire run**. nbd with `-connections 4` creates four blk-mq
hardware queues and steers each request by the submitting CPU; reclaim and
`swapoff` are concentrated enough that only two queues are ever used. Adding
connections does not help, and neither does removing them — it is kernel-side
mapping, outside this daemon's reach.

On the active connection during `swapoff`: 1,001,950 reads in 42 s at 43% busy
is a **42 µs cycle — roughly 18 µs in the daemon and 24 µs waiting for the
kernel to send the next request**. With `reqs/batch` pinned at 1.00 there is
nothing to overlap that wait with.

### 2.3 Per-page breakdown, after §3's codec change

Read (one 4 KiB page, `perf/unit-us`):

| | µs | share |
|---|---|---|
| `cuStreamSynchronize` | 9.88 | **57%** |
| reply `send` (two syscalls) | 2.76 | 16% |
| copy issue | 2.26 | 13% |
| decompress | 2.52 | 14% |
| header `recv` | 0.27 | 2% |
| **total** | **~17.4** | |

Write, per 64-block window on the oversized path (`handle_one` →
`store_blocks`, `nbd-vram.c:1683`):

| | µs | share |
|---|---|---|
| compress 64 blocks | 443 | **71%** |
| 64 copy launches | 128 | 20% |
| one stream sync | 55 | 9% |
| **total** | **~626** | |

A 433 KiB write — the size the kernel's reclaim actually issues, 105 blocks —
is two of those windows, which is the ~1.25 ms `lat-wr` p50.

---

## 3. What has been tried

### 3.1 `VRAM_COMPRESS=lz4` — large win, at a cost worth naming

| | zstd-1 | lz4 | |
|---|---|---|---|
| fill throughput | 71.6 MiB/s | **118.0 MiB/s** | +65% |
| compress | 15.86 µs/blk | **6.93 µs/blk** | −56% |
| decompress | 7.68 µs/blk | **2.52 µs/blk** | −67% |
| write latency p50 | ~1024 µs | **~512 µs** | −50% |
| `swapoff` peak | 105.5 MiB/s | 116.2 MiB/s | +10% |
| **compression ratio** | **4.11x** | **2.79x** | **−32%** |

The whole 9 GiB fill moved from two 60 s stats windows into one.

**The capacity cost is the part to watch.** At 2.70x measured, a 9216 MiB
logical device needs 3.33 GiB of the 3.38 GiB heap — **98.6% committed at a
full fill**, with occupancy already 98.6% so there is no fragmentation slack to
reclaim. zstd's 4.11x put the same fill at 65%. lz4 buys write speed by
spending essentially the entire ENOSPC margin, and any less-compressible
workload runs out.

If lz4 is kept, either drop `VRAM_DISK_SIZE_MB` to ~8192 or raise
`VRAM_SETUP_SIZE_MB`. §4.1 is the alternative: make zstd fast enough to keep.

### 3.2 `VRAM_CU_SCHED=spin` — no effect *(negative result)*

The hypothesis was that ~10 µs to synchronize a 4 KiB DtoH had to be
wait-strategy overhead, since the transfer itself crosses PCIe in well under a
microsecond, and that spinning would collect the completion sooner.

It did not. Sync went from 10.01 to 9.88 µs/copy — inside the noise — and the
histogram stayed a hard spike at 8–16 µs (`8us:1209324` of 1,248,501 syncs).

**Conclusion: the ~10 µs is the driver's submit-and-complete round trip for a
small copy, not how the completion is waited on.** Spinning on a completion
that has not been written yet does not make it arrive sooner. Do not re-propose
a wait-strategy change. The knob is kept (`VRAM_CU_SCHED`, default `auto`)
because it costs nothing and re-testing on another driver is one env var.

This is the single largest item in the read budget and it is, on this evidence,
a floor rather than an inefficiency.

### 3.3 Prefetching ahead of the kernel — dead for the case that matters

`perf/rd-locality` was added specifically to test this before building it.
During `swapoff` reads are 68–77% sequential, which would prefetch well. During
**normal demand paging they are 10–21% sequential and 75–85% land somewhere
else entirely**, which would not.

Since `swapoff` is the phase nobody cares about (§1), a prefetcher would spend
its effort on the wrong half of the benchmark. Not worth building. The
instrumentation stays so the conclusion can be re-checked on a different
workload.

---

## 4. Candidates: write path

This is where the headroom is. Compression is 71% of a write window and the
daemon is only ~35% busy on the active worker, so throughput is not
daemon-limited — but request latency feeds back into how fast the kernel's
reclaim will push, so cutting it is not purely cosmetic.

### 4.1 Parallelize compression across the idle workers *(largest, hardest)*

Two of four workers do nothing (§2.2). A 433 KiB write is 105 blocks compressed
serially on one thread. Splitting the window across three threads takes
compression from 443 µs to ~150 µs: **−47% per window.**

This is also the answer to §3.1's capacity problem — it is what would let zstd's
4.11x come back without giving up the write throughput lz4 bought.

Cost: a work queue the I/O path can use without allocating. The daemon's
existing invariant is that nothing on the swap path calls `malloc` (that is what
`PR_SET_IO_FLUSHER` and the pre-faulted per-worker arenas are for), so the pool
and its per-task state must be preallocated at startup like everything else.
Workers already hold their own codec contexts, so a helper compressing on behalf
of another worker must use its own, not the requester's.

### 4.2 `cuMemcpyBatchAsync` *(medium, availability-dependent)*

64 launches at 2.00 µs each is 128 µs per window, 2.3x the sync it feeds.
CUDA 12.8 added a batched memcpy entry point that submits many copies in one
call; 128 µs would become roughly one launch. **−19% per window.**

Fits the existing pattern exactly — the daemon already `dlopen`s libcuda and
probes optional symbols with fallback (`load_libcuda`, and `g_static_ctx` for
the codec). Verify the symbol name against the installed driver before
building; this is from memory and has not been checked.

### 4.3 `LEGACY_WINDOW_BLOCKS` 64 → 128 *(trivial)*

`nbd-vram.c:642`. `IO_BUF_SIZE` derives from it, so a 433 KiB request currently
splits into two receive-and-process cycles: two `recv` syscalls and two stream
syncs where one of each would do. Raising it to 128 makes the common reclaim
write a single cycle. **~−5%**, one constant, and about 1 MiB more host buffer
across four workers.

Check `w->maxops` interacts as expected (`nbd-vram.c:2213`) — it is already 512,
so the window is bounded by `IO_BUF_SIZE`, not by `maxops`.

### 4.4 Interleave compression with copy issue *(easy, modest)*

`store_blocks` compresses every block in the window, then issues every copy,
then syncs once. Issuing each copy as soon as its block is compressed lets the
DMA for block *i* overlap the compression of *i+1*.

Only the 55 µs sync can actually overlap — the 128 µs of launches is CPU work on
the same thread and does not — so this is **~−9%**, not the −30% the shape of
the loop suggests. Worth it because it is ~20 lines, but do not expect more.

The batched path (`flush_batch`, `nbd-vram.c:2047`) is harder to interleave than
the oversized path and should be left alone: it must be able to abort a
request's siblings when one block fails, which the current prepare-all-then-go
structure makes straightforward.

---

## 5. Candidates: read path

The realistic ceiling here is about 10%, because §3.2 established that 57% of
the read budget is a hardware/driver floor. Listed in the order worth trying.

### 5.1 A/B `cuMemcpyDtoH` against `cuMemcpyDtoHAsync` + synchronize

The only remaining shot at the 9.88 µs. The synchronous entry point may take a
different path in the driver for small transfers rather than pushing a command
and waiting on it. Behind an env knob like `VRAM_CU_SCHED`, measured with
`perf/sync`.

Might be worth nothing. It is ~30 lines and it is the one thing that could move
the dominant cost, so it should be tried before anything else on this list.

### 5.2 One `sendmsg` instead of two `send` calls

Every read reply is a 16-byte header then a 4096-byte payload, sent as two
syscalls (`nbd-vram.c:1946` on the per-request path, `nbd-vram.c:2098` in
`flush_batch`). A single `writev`/`sendmsg` saves roughly 1–1.5 µs of 17.4.
Reliable, small, no semantic change.

### 5.3 Adaptive skip of the non-blocking header probe

`recv_hdr_nb` (`nbd-vram.c:1378`) issues a `MSG_DONTWAIT` recv per batch to see
whether more requests are queued. At the measured depth of 1.00 it returns
EAGAIN essentially every time — 1.25 M wasted syscalls in one 42 s window.

Skipping it after a run of depth-1 batches saves ~0.3 µs. Marginal, and it
trades away the adaptivity that makes batching work the moment load arrives, so
it needs a decay that re-enables probing quickly. Probably not worth the risk.

---

## 6. Explicitly not worth doing

- **Anything about the allocator lock.** 0.0% contention over 1.8 M
  allocations (§2.1). The first analysis suspected this and was wrong.
- **A wait-strategy change for `cuStreamSynchronize`.** Measured, no effect
  (§3.2).
- **Prefetching ahead of the kernel.** Measured, wrong workload (§3.3).
- **More NBD connections.** Two of four are already idle (§2.2); the mapping is
  kernel-side.
- **A host-side cache of compressed blocks.** It would genuinely cut read
  latency — a hit skips the GPU round trip entirely — but it spends host RAM,
  which is the resource this daemon exists to conserve. Self-defeating.
- **Allocating one contiguous VRAM run per window** so a window's copies can be
  coalesced into a few large ones. The `clen` histogram is broad, so the blocks
  in a window land in many size classes and runs would be short anyway; forcing
  contiguity would mean abandoning the size-class allocator, whose whole design
  is O(1) individual frees. See `heap-occupancy.md` for why that structure is
  the way it is.

---

## 7. Suggested order, if this is picked up

1. **4.3 + 4.4** — an hour's work for ~14% of the write window, near-zero risk.
2. **5.1** — cheap experiment against the dominant read cost. If it does not
   move, the read path is finished short of an architecture change, and that is
   worth knowing before anything else is attempted.
3. **4.1** — the one that changes the picture, and the one that buys back the
   zstd capacity margin §3.1 gave up.

4.2 fits anywhere once its availability is confirmed.
