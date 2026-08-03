# Heap occupancy: reporting, allocation policy, and compaction

Status: §3 and §4 are implemented; §5 remains deferred.
Baseline: `nbd-vram.c` as of `develop` (`842d4df`). The measurements in §1 and
every line number quoted below are against that revision, not against the
implemented result — the "as implemented" subsections describe what changed.

This describes two changes worth making now (§3, §4) and one larger one deferred
(§5). It is written to be read cold, so §1 and §2 record the measurements that
motivate all three.

**Read §4.1 before trusting §4.** The allocation-policy change was implemented as
designed and measured against its own baseline, and it did not move occupancy.
The reasoning in §2 about *why* extents do not drain holds up; the conclusion
that a placement heuristic can fix it does not. That result changes what §5 is
for, so it is recorded rather than quietly dropped.

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

## 3. Change 1 — report occupancy *(implemented)*

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

5. **Warn on the fragmentation signature, not just on fullness.** The existing
   warning fires on `committed` percentage alone, which cannot distinguish "full
   of data" from "full of holes". Add a second condition — roughly
   `committed >= 75% && occupancy < 50%` — with the same hysteresis treatment as
   the existing one, and wording that names fragmentation as the cause.

6. **A second line describing the bins from §4**, printed on the same tick as
   the stats line. Where the occupancy figure says how bad the fragmentation is,
   this says what shape it has.

### As implemented

Two lines, each still one physical line, printed together by `stats_worker`:

```
[nbd-vram] stats: stored 2.00 GiB -> 87.0 MiB VRAM (23.54x) | codec 30.19x | occupancy 96.5% | extents 87/1024 (peak 87) | heap 87.0 MiB/1.00 GiB committed (8.5%) | slack 19.2% | raw 0.0% | enospc 0 trimmed 0 retries 0
[nbd-vram] stats bins: 87 used = 82 full + 5 partial | free 937 | classes 5 | partial by fullness: 75-100% 1 | 50-75% 1 | 25-50% 1 | 0-25% 2
```

(From the harness, not the field: the synthetic data compresses far better than
real anonymous memory.)

The bins line reports extent populations rather than bytes, and `classes` counts
size classes holding at least one partial extent. That last figure is there for
§2.1: holes are class-specific, so free space spread over 50 classes is much
weaker protection against ENOSPC than the same free space in five.

The fragmentation warning fires at `committed >= 75% && occupancy < 50%` and
clears at `committed < 65% || occupancy >= 60%`, mirroring the fullness warning's
hysteresis.

### Notes

- The `used`/`free` extent counts became `used`/`total` in the line, since the
  free count is on the bins line and `total` is what makes the ratio readable.
- `hsize()` did **not** need more buffers: rendering `commit` twice needs two
  buffers rather than one, but dropping the separate slot-bytes figure (it is
  `occupancy × committed`) freed one, so four still does.
- The high-water mark `g_extents_peak` is a `uint32_t` matching
  `g_extents_used`, not the `unsigned long` sketched above, and is updated in
  `slot_alloc` under `g_alloc_mu` on the fresh-extent path only — the only place
  `g_extents_used` grows.
- `commit` is guarded against divide-by-zero along with the other ratios.
- `stats bins:` deliberately does not contain the string `stats:`, so existing
  `grep stats:` invocations still match exactly one line per tick.

---

## 4. Change 2 — allocation policy *(implemented; see §4.1 for the result)*

Intended to fix the generational mixing described in §2. This is prevention, not
cure: it stops occupancy from collapsing, but it cannot recover extents that are
already fragmented. That is §5.

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

### Interaction with locality

Preferring the fullest extent naturally means "keep filling the one you are
already filling", which also preserves the temporal locality the kernel handed
us (§2). The two goals point the same direction here; no separate mechanism
needed.

### As implemented

`g_class_head[NCLASS]` became `g_class_bin[NCLASS][NBINS]` with `NBINS` 4, plus
`g_bin_extents[NBINS]` for the stats line. The bin index is derived from
`nfree`/`nslots` on demand rather than cached in the extent, so there is no new
per-extent state to keep in sync and `struct extent` is unchanged at 16 bytes;
callers read `ext_bin()` before mutating `nfree` and hand the result to
`ext_rebin()` afterwards. Total new memory is the 1 KiB predicted.

Setting `NBINS` to 1 reproduces the old single-list behaviour exactly — every
partial extent bins to 0, and the full↔partial transitions become the original
head push and head removal. That is how the A/B below was run, so both sides
shared identical stats code.

---

## 4.1 What it actually did: nothing measurable

`test-harness/run.sh --fragment` (new, see `test-harness/fragment.py`) runs
rolling generations of 2 MiB clusters — written together, trimmed together,
lifetimes overlapping — and reads back the occupancy from §3. Occupancy after the
final drain, same workload, same seed, three placement policies:

| Policy | Default workload (51 classes) | Concentrated (8 classes) |
|---|---|---|
| Single partial list per class (the old behaviour) | 64.9% | 81.4% |
| Fullest bin first (this change) | 62.4% | 81.4% |
| Emptiest bin first (the inverse) | 62.6% | 81.7% |

Three policies, one of them the deliberate inverse of the other, land within two
points of each other, and on the concentrated workload two of them are identical
to the digit. Committed extents receded from their peak by the same handful in
every run. Whatever governs occupancy here, it is not which partial extent gets
topped off.

### Why

1. **Placement never gets to choose "neither".** `slot_alloc` commits a fresh
   extent only when its class has *no* partial extent at all. So while any hole
   exists in the class, every new block goes into one. The policy picks which
   extent gets contaminated by a foreign generation; it cannot decline to
   contaminate one.

2. **Per cycle, allocations ≈ holes.** A steady working set frees about as many
   slots per cycle as the next generation needs. Every partial extent in a class
   is therefore visited over a cycle whatever the order, and a single foreign
   block is enough to keep an extent off the free list.

3. **Fullest-first *is* the old behaviour, for the extents that matter.** §2's
   complaint was that an extent going full→partial is pushed to the head and
   becomes the next allocation target. An extent that has just lost one slot is
   by definition ≥75% full, so it lands in bin 0 and is the next allocation
   target. Binning only diverges from the old policy once an extent has lost a
   quarter of its slots — by which point it has already been re-contaminated.

Point 3 is the one that should have been caught at design time: the mechanism in
§2 was correctly identified and the proposed fix does not disturb it.

### What this does not overturn

- §2's diagnosis. Extents still fail to drain because they hold blocks from
  several generations; that is visible directly in the bins line, where partial
  extents spread broadly across all four buckets instead of piling into the
  full and empty ends.
- §1.2 and §5. Nothing here recovers stranded space; it never claimed to.

### Kept anyway

The change is retained rather than reverted, for reasons that are worth being
explicit about since it did not deliver what it promised:

- The bins line (§3) is the only view of heap shape there is, and it needs the
  bins to exist.
- Fullest-first is a sound default independent of drainage: it finishes extents
  off instead of leaving a spread of half-used ones, which is what keeps the
  larger-class fallback in §2.1 from being reached prematurely.
- It costs 1 KiB and a comparison per allocation, both measured as noise.
- §5 needs a way to say "allocate, but not into a fresh extent"; a class's bins
  are exactly the structure that makes that expressible.

If a future change wants the list machinery gone, the honest summary is that it
buys visibility and costs nothing, not that it fixed fragmentation.

### Success criterion, revised

The original criterion — occupancy above ~70% after fill/drain/refill, against a
27.6% baseline — was not met and is not reachable by this change. `--fragment`
now asserts a regression *floor* (55%) rather than a target, and prints the
number for comparison. `slack` was checked as planned and did not move: 1.5% on
both sides of the A/B, so the bin-to-class mapping is sound.

---

## 5. Future work — compaction

Deferred, but it is the load-bearing fix and the other two changes should be
read as preparation for it. §4.1 raises its priority rather than lowering it:
with the allocation-policy lever measured and found inert, compaction is no
longer the more thorough of two options, it is the only one left that addresses
stranded space.

### Why it is not optional

No allocation policy recovers an already-fragmented heap — and, per §4.1, no
allocation policy measurably prevents one either. At 27.6% occupancy,
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
restrict the allocation or accept and bound the transient. The bins from §4 make
the restriction cheap to express: a `slot_alloc` variant that consults
`class_pick` and returns `SLOT_NONE` instead of falling through to `g_free_head`
is a few lines, and taking from bin 0 first is the right target for relocated
blocks anyway.

**Throttling and triggering.** A sweep burns PCIe bandwidth, VRAM bandwidth and
the alloc mutex, all of which are on the swap path. It should run from the
existing stats thread or a sibling, back off under load, rate-limit blocks moved
per second, and trigger on the occupancy signal from §3 rather than on a timer —
something like "occupancy < 50% and committed > 60%", stopping once occupancy
recovers.

**Occupancy under-reports the problem when §1.1 blocks are present.** This came
out of modelling undiscarded frees in the harness (`FRAG_STALE_PCT`) and it is
not obvious: stale blocks *raise* occupancy. The daemon cannot tell them from
live ones, so they count in `g_slot_bytes` — a heap that is 60% full of data
nobody will ever read again reports 60% occupancy and looks healthy. The failure
in §1 was visible only because the peak had committed so many extents that even
the stale blocks could not fill them.

Two consequences for the trigger. First, an occupancy threshold will not fire on
the heap that most needs help, so pair it with something that notices the
divergence — `committed` sitting near its high-water mark while `trimmed` climbs
is the signature. Second, compaction will faithfully relocate stale blocks,
spending PCIe bandwidth to preserve data the kernel freed long ago. That is
correct behaviour and unavoidable from inside the daemon, but it means the
bandwidth budget should be sized against `stored`, not against the fraction of
`stored` that is useful.

### A second lever, if compaction stays deferred

§4.1 rules out *choosing between* partial extents. It says nothing about
declining to use one. Committing a fresh extent while partials remain — bounded
by free-extent headroom, so it degrades to current behaviour as the heap fills —
would let a generation land in extents of its own and drain as a unit, which is
the thing the current allocator structurally cannot do (§4.1, point 1). It
trades committed VRAM up front for extents that can actually be released later,
so it is only worth having if something eventually collects them, which is
compaction again. Worth measuring with `--fragment` before it is worth
designing: the harness makes it a one-line experiment.

---

## 6. Validation

Wired to the host-side harness as `test-harness/run.sh --fragment`, which drives
rolling generations of 2 MiB clusters over the NBD socket and reads the numbers
back out of the §3 stats line. It also re-reads live and trimmed clusters, which
is a genuine check on the bin list surgery — a botched relink hands out a slot
that is already in use, and the symptom is one block reading back as another.
`SRC=... ./run.sh --fragment` builds a different source file, which is how the
§4.1 A/B was run.

The metrics that matter, all exposed by §3:

| Metric | Field baseline | Harness | Target |
|---|---|---|---|
| Occupancy after fill/drain/refill | 27.6% | 62-66%, policy-independent | > 70% |
| Effective ratio (`stored/committed`) | 0.98x | 1.2x | > 2.67x (the configured 8192/3072) |
| `committed` high-water after a drain | 2.68 GiB | 321 → 305-317 extents | recedes |
| `slack` | 2.7% | 1.5%, unchanged across policies | unchanged |
| `retries` | 0 | 0 | non-zero under compaction, but bounded |

**The harness does not reproduce the field failure.** No configuration tried gets
occupancy below ~60%, against 27.6% observed in the field. The workload holds a
flat working set, so `committed` never builds the high-water mark that the
collapse is measured against; reproducing it needs a large peak followed by a
scattered drain. Until that exists, a green `--fragment` run means "placement did
not regress", not "fragmentation is handled". Compaction will need real-hardware
validation regardless — the stub models neither DtoD copies nor their cost.

The shape of the test that produced the numbers in §1, still the reference:
fill swap with large processes, drain, repeat several times, then exit everything
and read the final stats line. Occupancy after the final drain is the headline
number — it is where the failure is most visible and where a fix will show up
first.

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
