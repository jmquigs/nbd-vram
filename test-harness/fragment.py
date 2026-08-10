#!/usr/bin/env python3
"""Measure extent occupancy under a rolling fill/drain/refill workload.

This is the test for the allocation policy, not for correctness. It reproduces
the shape of the workload that collapsed occupancy to 27.6% in the field (see
docs/heap-occupancy.md): several "processes" whose pages are written together,
live for a while, and are then freed together, with their lifetimes overlapping.

Each cycle writes one generation of 2 MiB clusters - 2 MiB is SWAPFILE_CLUSTER,
the granularity at which the kernel actually issues discards - and trims the
generation from LIVE_GENS cycles ago. Cluster numbers are drawn at random, so
generations interleave in the address space exactly as processes do.

The number that matters at the end is occupancy: slot bytes as a fraction of
committed VRAM. A working set that is stable in size should sit in extents that
are mostly full. If it does not, freed slots are scattered across extents that
can never empty, and committed VRAM never recedes.

Data is composed from a fixed pool of pages spanning a range of compressibility,
so compressed lengths land across many size classes rather than piling into one.
That matters: holes are class-specific, and a test that used a single class
would not see the failure at all.

What it has actually shown so far is that the allocator's *placement* policy is
not the lever: single partial list, fullest-bin-first and emptiest-bin-first all
settle within two points of each other. Treat the occupancy number as a
regression signal on the allocator as a whole, not as a score for the placement
heuristic. FRAG_* environment variables tune the workload; SRC=... on run.sh
builds a different source, which is how those A/Bs were run.
"""
import os
import random
import re
import sys
import time

sys.path.insert(0, sys.path[0] or '.')
from nbdtest import NBD, compressible, incompressible

CLUSTER      = 2 << 20  # SWAPFILE_CLUSTER on x86-64: the kernel's discard unit
GEN_CLUSTERS = int(os.environ.get("FRAG_GEN_CLUSTERS", 96))   # one generation
CYCLES       = int(os.environ.get("FRAG_CYCLES", 24))
LIVE_GENS    = int(os.environ.get("FRAG_LIVE_GENS", 2))       # generations alive at once
POOL_PAGES   = int(os.environ.get("FRAG_POOL_PAGES", 64))

# Percentage of each dying generation that is freed by the kernel but never
# discarded, so the daemon keeps holding it. This is not a hypothetical: with
# --discard=pages the kernel only issues a TRIM when an entire 2 MiB swap
# cluster becomes free, and scattered frees generate none at all. Blocks like
# these are permanent occupants of whatever extent they landed in, which is why
# they are worth modelling separately from the churn.
STALE_PCT    = int(os.environ.get("FRAG_STALE_PCT", 0))

# A regression floor, not an aspiration. The default workload settles at 62-66%
# occupancy and does so for every allocation policy measured so far (see
# docs/heap-occupancy.md §4.1), so the useful thing to assert is that a change
# did not make placement worse. The number to look at is the one printed.
OCCUPANCY_FLOOR = float(os.environ.get("FRAG_OCCUPANCY_FLOOR", 55))


def block_pool():
    """Pages spanning a range of compressibility, one 4 KiB block each.

    Real anonymous memory does not compress uniformly, and the allocator's
    behaviour depends on that spread: each distinct compressed length routes to
    its own size class, and a class can only borrow slots upwards.
    """
    pool = []
    for i in range(POOL_PAGES):
        rand_bytes = (4096 * (i + 1) // (POOL_PAGES + 2)) & ~63
        pool.append(incompressible(rand_bytes, 0xA000 + i) +
                    compressible(4096 - rand_bytes, 0xB000 + i))
    return pool


def cluster_data(pool, cl):
    """Deterministic content for cluster `cl`, so it can be re-read and checked."""
    return b"".join(pool[(cl * 7 + j) % len(pool)] for j in range(CLUSTER // 4096))


def stats_lines(path):
    """Complete stats lines in the daemon log. The trailing newline check matters:
    the log is read while the daemon is writing to it."""
    try:
        with open(path, "r", errors="replace") as f:
            return [ln for ln in f if "] stats: " in ln and ln.endswith("\n")]
    except FileNotFoundError:
        return []


def wait_for_stats(path, timeout=40):
    """Wait for a stats line printed after this call, i.e. after the workload.

    The daemon reports on a timer, so lines already in the log describe the heap
    from somewhere in the middle of the run - taking the newest of those reads
    like a finished measurement and is not one. This cost a confusing A/B.
    """
    already = len(stats_lines(path))
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = stats_lines(path)
        if len(lines) > already:
            return lines[-1].rstrip()
        time.sleep(0.5)
    return None


def parse_stats(line):
    def num(pat, cast=float):
        m = re.search(pat, line)
        return cast(m.group(1)) if m else None
    return {
        "stored":    num(r"stored ([\d.]+ \w+) ->", str),
        "vram":      num(r"-> ([\d.]+ \w+) VRAM", str),
        "effective": num(r"VRAM \(([\d.]+)x\)"),
        "codec":     num(r"codec ([\d.]+)x"),
        "occupancy": num(r"occupancy ([\d.]+)%"),
        "used":      num(r"extents (\d+)/", int),
        "total":     num(r"extents \d+/(\d+)", int),
        "peak":      num(r"peak (\d+)\)", int),
        "slack":     num(r"slack ([\d.]+)%"),
        "enospc":    num(r"enospc (\d+)", int),
    }


def main(sock, log):
    c = NBD(sock)
    pool = block_pool()
    nclusters = c.size // CLUSTER
    rnd = random.Random(20260803)

    print(f"device {c.size >> 20} MiB = {nclusters} clusters of {CLUSTER >> 20} MiB; "
          f"{CYCLES} cycles of {GEN_CLUSTERS} clusters, {LIVE_GENS} generations live")
    if nclusters < GEN_CLUSTERS * (LIVE_GENS + 1):
        print("  FAIL: device too small for the working set")
        return 1

    failures = []
    free = list(range(nclusters))
    rnd.shuffle(free)
    live = []          # generations, oldest first
    stale = []         # written, logically dead, never discarded - see STALE_PCT
    trimmed = []       # clusters freed by the most recent drain

    t0 = time.time()
    for cycle in range(CYCLES):
        gen = [free.pop() for _ in range(GEN_CLUSTERS)]
        for cl in gen:
            e = c.write(cl * CLUSTER, cluster_data(pool, cl))
            if e != 0:
                failures.append(f"write to cluster {cl} failed with errno {e}")
                break
        live.append(gen)

        if len(live) > LIVE_GENS:
            dying = live.pop(0)
            keep = len(dying) * STALE_PCT // 100
            stale += dying[:keep]              # freed by the kernel, never discarded
            trimmed = dying[keep:]
            for cl in trimmed:
                c.trim(cl * CLUSTER, CLUSTER)
            free.extend(trimmed)
            rnd.shuffle(free)
        if failures:
            break
    c.flush()
    dt = time.time() - t0
    written = CYCLES * GEN_CLUSTERS * CLUSTER
    print(f"  {CYCLES} cycles, {written >> 20} MiB written in {dt:.1f}s" +
          (f"; {len(stale) * CLUSTER >> 20} MiB of the dead was never discarded"
           if stale else ""))

    # Correctness net. The bin lists are surgery on a doubly-linked list under
    # the alloc mutex; a botched relink hands out a slot that is already in use,
    # and the symptom is one block reading back as another.
    live_flat = [cl for gen in live for cl in gen]
    bad = [cl for cl in rnd.sample(live_flat, 12)
           if c.read(cl * CLUSTER, CLUSTER) != cluster_data(pool, cl)]
    print(f"  re-read 12 live clusters: {'all correct' if not bad else f'MISMATCH at {bad}'}")
    if bad:
        failures.append("live data did not read back correctly after the cycles")

    if trimmed:
        zeros = bytes(CLUSTER)
        stale = [cl for cl in rnd.sample(trimmed, 4) if c.read(cl * CLUSTER, CLUSTER) != zeros]
        print(f"  re-read 4 trimmed clusters: {'all zero' if not stale else f'STALE at {stale}'}")
        if stale:
            failures.append("trimmed clusters did not read back as zeros")

    line = wait_for_stats(log)
    if line is None:
        print(f"  FAIL: no stats line appeared in {log} - is VRAM_STATS_INTERVAL_SEC set?")
        failures.append("no stats line to measure")
    else:
        s = parse_stats(line)
        if s["occupancy"] is None:
            failures.append(f"could not parse the stats line: {line}")
        else:
            print(f"  live {s['stored']} in {s['vram']} of VRAM "
                  f"({s['effective']:.2f}x effective, {s['codec']:.2f}x codec)")
            print(f"  occupancy {s['occupancy']:.1f}% | extents {s['used']}/{s['total']} "
                  f"(peak {s['peak']}) | slack {s['slack']:.1f}% | enospc {s['enospc']}")
            if s["occupancy"] < OCCUPANCY_FLOOR:
                failures.append(f"occupancy {s['occupancy']:.1f}% is below the "
                                f"{OCCUPANCY_FLOOR:.0f}% regression floor - committed "
                                f"VRAM is holding more holes than it used to")
            if s["peak"] and s["used"] / s["peak"] > 0.9 and s["occupancy"] < 90:
                # Not a failure on its own, but the pairing is the signature.
                print("  note: committed extents never receded from their peak")

    c.close()
    for f in failures:
        print(f"  FAIL: {f}")
    print("FRAGMENTATION TEST PASSED" if not failures
          else f"{len(failures)} FRAGMENTATION FAILURE(S)")
    return len(failures)


if __name__ == "__main__":
    log = sys.argv[2] if len(sys.argv) > 2 else "daemon.log"
    sys.exit(1 if main(sys.argv[1], log) else 0)
