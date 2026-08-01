# Heap occupancy: reporting, allocation policy, and compaction

Status: design notes, nothing implemented yet.
Baseline: `nbd-vram.c` as of `develop` (`842d4df`).

This describes two changes worth making now (§3, §4) and one larger one deferred
(§5). It is written to be read cold, so §1 and §2 record the measurements that
motivate all three.

---

## 1. The observation

After a session of repeatedly filling and draining swap with large processes,
then exiting all of them:

```
$ swapon --show
NAME       TYPE      SIZE   USED PRIO
/dev/zram0 partition  15G  19.5M 1000
/dev/nbd0  partition   8G 718.8M 1500

$ tail -1 /tmp/nbd-vram.log
[nbd-vram] stats: stored 2.63 GiB -> 757.4 MiB on device (3.55x)
  | heap 2.68 GiB/3.00 GiB committed (89.3%)
  | slack 2.7% | raw 1.3% | enospc 0 trimmed 2717220 retries 0
```

Config: `VRAM_DISK_SIZE_MB=8192`, `VRAM_SETUP_SIZE_MB=3072`,
`swapon --discard=pages`, `nbd-client -connections 4`.

Two things are wrong with this picture, and they are independent.

### 1.1 The daemon holds ~1.93 GiB of data the kernel already freed

The kernel reports 718.8 MiB in use (184,000 slots). The daemon reports 2.63 GiB
live (689,400 blocks). The ~505,000-block difference is data belonging to swap
slots the kernel freed without telling us.

This is not an accounting bug. `wr_publish()` (`nbd-vram.c:1189`) frees exactly
the slot it displaces under the block's stripe lock and only increments
`g_blocks_live` on the unwritten→written transition; `trim_range()`
(`nbd-vram.c:1541-1545`) decrements all counters symmetrically; `wr_abort()`
releases unpublished reservations. And a leak is structurally impossible:
`g_index` is a fixed array of `g_nblocks` entries holding at most one slot each,
so `stored` is bounded by the device size by construction.

The cause is kernel discard granularity. `--discard=pages` does **not** discard a
swap slot when it is freed — freed slots are marked available in the swap map and
nothing more. A discard is issued only when an entire swap cluster
(`SWAPFILE_CLUSTER`, 512 pages / 2 MiB on x86-64 with THP swap) becomes
completely free, at which point `free_cluster()` routes it to the discard list.
Scattered frees generate no TRIM, and with swap pressure at zero nothing reuses
those slots either, so no overwrite reclaims them.

Nothing can be done about this from inside the daemon — NBD has no way to ask
"is this block still live?". It is recorded here because it explains why
occupancy collapses so hard after a drain, and because it will otherwise get
rediscovered as a suspected leak.

*Worth confirming once:* log the size and alignment of incoming TRIM requests. If
they are all ≥2 MiB and 2 MiB-aligned, that is `SWAPFILE_CLUSTER` and this
explanation is settled.

### 1.2 Extent occupancy has collapsed to 27.6%

```
slot bytes    757.4 MiB
committed     2.68 GiB   (2745 extents of 3072; 327 free)
              --------
occupancy     27.6%      (was 81.9% in an earlier, near-full test)
dead space    1.94 GiB   inside committed extents
```

End-to-end this means:

```
stored / committed        2.63 / 2.68  =  0.98x
useful (kernel) / VRAM    0.70 / 2.68  =  0.26x
```

2.68 GiB of VRAM is being spent to hold 718.8 MiB of swap the kernel still wants.
The codec earns 3.55x and the allocator gives all of it back.

`committed` is a high-water mark: it tracks *peak concurrent* slot demand and
never recedes, because an extent is only recycled when every slot in it is free
(`nbd-vram.c:816`, where this is called out as a deliberate choice).

---

## 2. Why the extents do not drain

Two mechanisms, both in `slot_alloc`/`slot_free`.

**Generational mixing.** `slot_free()` at `nbd-vram.c:957`:

```c
if (e->nfree == 0) lst_push(&g_class_head[e->cls], ei);
```

An extent going full→partial is pushed onto the *head* of its class list, and
`slot_alloc()` takes the head (`nbd-vram.c:914`). So the most recently freed
extent becomes the immediate next allocation target. When a process exits and
trims thousands of slots, its extents jump to the front of their class lists and
the next process's writes are sprinkled into them. Those extents can then never
reach `nfree == nslots`. Every start/stop cycle grows the population of
sparse-but-not-empty extents.

**Discarded locality.** Every TRIM we receive is a contiguous 512-block run whose
pages were allocated together and died together — the kernel has already done the
lifetime-clustering work. Size-class routing sprays those 512 slots across up to
512 different extents. At the measured average compressed size of 1152 B
(class 17, `nslots` = 910), an extent only empties if 910 unrelated blocks
happen to die at once. In practice that never happens.

Note the shape of this: the kernel reclaims a 2 MiB cluster only when all 512
slots are free and never relocates; the allocator reclaims a 1 MiB extent only
when all slots are free and never relocates. Same policy at two layers, and each
layer's residue becomes the next layer's input.

### 2.1 A latent ENOSPC hazard

`slot_alloc()` can borrow a slot from a **larger** class when its own class is
empty (`nbd-vram.c:925-929`) but never from a smaller one. So holes are
class-specific. At 89.3% committed with 327 free extents and 1.94 GiB of holes
concentrated in low classes, a refill wave of poorly-compressing pages — which
needs high classes — can hit ENOSPC while the heap is 72% empty. `enospc` is
still 0, but the configuration needs 2.67x (8192/3072) and is currently
delivering 0.98x end-to-end.

---

## 3. Change 1 — report occupancy

Cheapest change here and the highest information gain. The current line reports
`stored/slots` as its headline ratio, which is not the quantity that governs
ENOSPC — `heap_committed()` is. During the run above it read a reassuring
`3.55x` while the device was at 0.98x effective.

### Scope

`stats_log()` (`nbd-vram.c:1957`) and the warning block in `stats_worker()`
(`nbd-vram.c:1993-2002`). No new state on the I/O path; every quantity below is
already tracked.

### Changes

1. **Headline ratio becomes `stored / committed`.** This is the only ratio that
   predicts when writes start failing.
2. **Add `occupancy` = `slots / committed`.** The single number that would have
   made the situation above obvious at a glance.
3. **Keep the codec ratio as a secondary figure** (`stored / codec_bytes`) — it
   is what zstd actually earns and it is useful to see it diverge from the
   effective ratio. That divergence *is* the fragmentation.
4. **Add extent counts** (`used`/`free`) and a high-water mark for
   `g_extents_used`. The high-water mark is one new `unsigned long` updated in
   `slot_alloc` under `g_alloc_mu`, which is already held.

Target shape:

```
[nbd-vram] stats: stored 2.63 GiB -> 2.68 GiB VRAM (0.98x) | codec 3.62x
  | occupancy 27.6% | extents 2745/3072 (peak 2745)
  | heap 2.68/3.00 GiB (89.3%) | slack 2.7% | raw 1.3%
  | enospc 0 trimmed 2717220 retries 0
```

5. **Warn on the fragmentation signature, not just on fullness.** The existing
   warning fires on `committed` percentage alone, which cannot distinguish "full
   of data" from "full of holes". Add a second condition — roughly
   `committed >= 75% && occupancy < 50%` — with the same hysteresis treatment as
   the existing one, and wording that names fragmentation as the cause.

### Notes

- `hsize()` renders into caller-supplied buffers; the target line needs more
  than the current four, so bump the array.
- Keep the line to one physical line. It is grepped and tailed.
- The ratios are already guarded against divide-by-zero; `commit` needs the same
  guard, as it is zero before the first allocation.

---

## 4. Change 2 — allocation policy

Fixes the generational mixing described in §2. This is prevention, not cure: it
stops occupancy from collapsing, but it cannot recover extents that are already
fragmented. That is §5.

### Scope

`slot_alloc()`, `slot_free()`, `ext_take()`, and the `g_class_head[]` array, all
in the block at `nbd-vram.c:845-970`. Entirely under `g_alloc_mu`, which is
already held on both paths. No change to the index, the read path, or the wire
protocol.

### Approach: fullness-binned partial lists

Replace `g_class_head[NCLASS]` with `g_class_bin[NCLASS][NBINS]`, bucketing each
class's partial extents by how full they are. Four bins is a reasonable start
(≥75% full, ≥50%, ≥25%, <25%), keyed off `nfree`/`nslots`.

- **Allocate from the fullest non-empty bin.** Nearly-full extents get topped off
  and retire from the lists; nearly-empty extents are left alone so that
  in-flight trims can finish emptying them. This is the whole idea: keep sparse
  extents sparse.
- **On alloc and free, move the extent between bins** when it crosses a
  boundary. `lst_remove`/`lst_push` are already O(1) on a doubly-linked list, so
  this is a couple of integer ops plus at most one relink.
- **Preserve the larger-class fallback** in its current form, scanning bins
  fullest-first within each candidate class.

Cost: `NCLASS × NBINS × 4` bytes = 1 KiB of new bookkeeping. This matters —
`nbd-vram.c:810-813` records that allocator memory comes out of the RAM the
daemon exists to conserve, and that constraint should not be relaxed casually.

### Cheaper variant, if an A/B is wanted first

Adding `g_class_tail[NCLASS]` and pushing full→partial extents to the tail rather
than the head is a ~5-line change that captures part of the benefit: freshly
freed extents go to the back of the queue and get time to drain. It is strictly
weaker than binning — pure FIFO will still refill a nearly-empty extent once it
reaches the front — but it is a useful control when measuring.

### Interaction with locality

Preferring the fullest extent naturally means "keep filling the one you are
already filling", which also preserves the temporal locality the kernel handed
us (§2). The two goals point the same direction here; no separate mechanism
needed.

### Success criterion

Occupancy after a fill/drain/refill cycle, measured by §3. Baseline is 27.6%.
Anything above ~70% would mean the mixing is fixed. Watch `slack` at the same
time — binning should not move it, and if it does something is wrong with the
bin-to-class mapping.

---

## 5. Future work — compaction

Deferred, but it is the load-bearing fix and the other two changes should be
read as preparation for it.

### Why it is not optional

No allocation policy recovers an already-fragmented heap. At 27.6% occupancy,
1.94 GiB is stranded inside committed extents and only *moving blocks* frees it.
Concretely, from the measurements in §1:

- Compaction alone: `committed` 2.68 GiB → ~0.76 GiB.
- If the stale blocks of §1.1 were also reclaimed: → ~0.21 GiB.

Note the ordering. Trimming every stale block would return **zero** VRAM on its
own, because freeing scattered slots does not empty extents — it would only drop
occupancy from 27.6% to 7.4%. Compaction is what converts freed slots into freed
VRAM.

### Why it is tractable here

The hard primitive already exists. Blocks are reached through `g_index`, not by
address, and every entry carries a generation counter that readers re-check after
their DtoH completes (`nbd-vram.c:745-795`). Relocation is exactly the race that
scheme was built for, and the retry path is already exercised in production —
`g_read_retry` is reported in the stats line.

### Sketch

Per block, under the block's stripe lock:

1. `slot_alloc()` a new slot outside the extent being drained.
2. Device-to-device copy, old → new; synchronize.
3. Take `IDXL(blk)`; re-read the entry.
4. If the grain no longer matches, the block was rewritten or trimmed underneath
   us — abort and `slot_free()` the new slot.
5. Otherwise publish `ent_make(new_grain, clen, gen + 1)` and release the lock.
6. `slot_free()` the old slot.

A reader that snapshotted the old entry and is mid-DtoH sees the generation bump
on re-check and retries. Note that step 6 can hand the old slot to a writer while
that reader is still DMAing from it: the reader will copy garbage into its
bounce buffer and then discard it on the generation mismatch. That is safe, but
it looks like a bug on first reading and should be commented as such.

### Open problems

**The reverse map.** `g_index` maps block → grain. Compaction needs grain →
block, and the extent bitmaps do not record ownership. The options:

- A global `grain → block` array: ~50M grains × 4 B = 200 MiB. Rejected on the
  memory constraint.
- Per-extent owner arrays: worst case 16384 slots × 8 B × 3072 extents =
  393 MiB. Rejected for the same reason.
- **Scan `g_index` and bucket live blocks by extent.** For an 8 GiB device that
  is 2M entries / 16 MiB, a few milliseconds at memory bandwidth. Too slow per
  extent, fine once per compaction cycle if the sweep targets many extents at
  once. This is the option to take: it costs no additional memory.
- A block-number header inside each slot: ~0.5% VRAM overhead at the measured
  average size, but recovering it requires a DtoH read per slot.

**`cuMemcpyDtoDAsync` is not currently loaded.** `load_cuda()`
(`nbd-vram.c:100-107`) resolves only `HtoD`/`DtoH`. Compaction needs
`cuMemcpyDtoDAsync_v2` added to the `LOAD_SYM` list, and it should be treated as
optional at load time so a driver lacking it degrades to compaction-disabled
rather than failing startup.

**Compaction must not raise `committed`.** Step 1 has to allocate from an
existing partial extent; if it is allowed to commit a fresh extent, a compaction
pass can transiently increase the very number it exists to reduce. Either
restrict the allocation or accept and bound the transient.

**Throttling and triggering.** A sweep burns PCIe bandwidth, VRAM bandwidth and
the alloc mutex, all of which are on the swap path. It should run from the
existing stats thread or a sibling, back off under load, rate-limit blocks moved
per second, and trigger on the occupancy signal from §3 rather than on a timer —
something like "occupancy < 50% and committed > 60%", stopping once occupancy
recovers.

---

## 6. Validation

To be wired to the test harness once that lands. The metrics that matter, all
exposed by §3:

| Metric | Baseline | Target |
|---|---|---|
| Occupancy after fill/drain/refill | 27.6% | > 70% |
| Effective ratio (`stored/committed`) | 0.98x | > 2.67x (the configured 8192/3072) |
| `committed` high-water after a drain | 2.68 GiB | recedes |
| `slack` | 2.7% | unchanged |
| `retries` | 0 | non-zero under compaction, but bounded |

The shape of the test that produced the numbers in §1: fill swap with large
processes, drain, repeat several times, then exit everything and read the final
stats line. Occupancy after the final drain is the headline number — it is where
the failure is most visible and where a fix will show up first.

An additional check worth having: force a refill after the drain and confirm
`stored` grows more slowly than kernel swap usage, which is the signature of
stale blocks (§1.1) being overwritten rather than accumulating.

### Other levers, for context

Independent of any code change:

- **Swap priority.** `nbd0` at 1500 sits above `zram0` at 1000, so the
  oversubscribed, fragmentation-prone tier fills first and stays pinned near
  full. Fragmentation only bites near full. Putting zram first keeps hot pages in
  RAM and keeps `nbd0` in the occupancy range where extents still drain.
- **`VRAM_DISK_SIZE_MB`.** Sizing should target the *effective* ratio, not the
  codec ratio. Until §4 and §5 land, the effective ratio after a drain cycle is
  the number to budget against.
