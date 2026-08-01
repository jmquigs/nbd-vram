#!/usr/bin/env python3
"""Fill the device and check what the daemon does about capacity.

Two modes, and they test opposite things:

  compressible  data that compresses like real anonymous memory. The whole
                logical device should fit in the smaller physical VRAM behind
                it - this is the feature working.

  random        incompressible data, stored raw. Writing should stop at roughly
                the physical VRAM size with ENOSPC. What matters is not that it
                fails but HOW: the connection stays up and everything already
                written is still readable and correct.

Both fills are generated deterministically from the offset, so any block can be
regenerated and compared afterwards without keeping gigabytes in memory.
"""
import sys, time, random

sys.path.insert(0, sys.path[0] or '.')
from nbdtest import NBD, compressible, incompressible

CHUNK = 1 << 20


def gen(mode, off):
    """Content for the CHUNK-sized block at `off`. Deterministic in both modes."""
    return compressible(CHUNK, off) if mode == "compressible" else incompressible(CHUNK, off)


def main(sock, mode):
    c = NBD(sock)
    print(f"device {c.size >> 20} MiB, filling with {mode} data")

    written, off, enospc_at = 0, 0, None
    t0 = time.time()
    while off + CHUNK <= c.size:
        e = c.write(off, gen(mode, off))
        if e != 0:
            enospc_at = off
            print(f"  write refused at {off >> 20} MiB with errno {e} "
                  f"({'ENOSPC' if e == 28 else 'unexpected'})")
            break
        written += CHUNK
        off += CHUNK
    dt = time.time() - t0
    print(f"  wrote {written >> 20} MiB in {dt:.1f}s "
          f"({written / dt / 1e6:.0f} MB/s, single stream)")

    failures = []
    if enospc_at is None:
        print(f"  filled the whole {c.size >> 20} MiB device without running out")
        if mode == "random":
            failures.append("incompressible fill never hit ENOSPC - is the device "
                            "overcommitted? set VRAM_DISK_SIZE_MB above VRAM_SETUP_SIZE_MB")
    else:
        if enospc_at != 0 and mode == "compressible":
            failures.append(f"compressible fill hit ENOSPC at {enospc_at >> 20} MiB")

        # The contract that actually matters after a full device: the connection
        # is still usable and nothing already stored was damaged. Sample across
        # the whole written range rather than trusting one probe.
        probes = [p for p in range(0, enospc_at, max(CHUNK, enospc_at // 16))]
        bad = []
        for p in probes:
            try:
                if c.read(p, CHUNK) != gen(mode, p):
                    bad.append(p >> 20)
            except OSError as ex:
                bad.append(f"{p >> 20}(errno {ex.errno})")
        print(f"  re-verified {len(probes)} probes across the written range: "
              f"{'all correct' if not bad else 'MISMATCH at ' + str(bad[:5]) + ' MiB'}")
        if bad:
            failures.append("data written before ENOSPC did not read back correctly")

        # Liveness: a real round trip on a fresh offset, not a truthy check.
        canary = incompressible(4096, 0xC0FFEE)
        try:
            probe_off = 0
            if c.write(probe_off, canary) != 0:
                # Overwriting an existing block can legitimately still fail when
                # full, since the new slot is reserved before the old is released.
                print("  connection alive after ENOSPC: yes (overwrite still refused, expected)")
            elif c.read(probe_off, 4096) == canary:
                print("  connection alive after ENOSPC: yes (write+read round trip ok)")
            else:
                failures.append("post-ENOSPC round trip returned wrong data")
        except OSError as ex:
            failures.append(f"connection unusable after ENOSPC: errno {ex.errno}")

    c.close()
    for f in failures:
        print(f"  FAIL: {f}")
    print("CAPACITY TEST PASSED" if not failures else f"{len(failures)} CAPACITY FAILURE(S)")
    return len(failures)


if __name__ == "__main__":
    mode = sys.argv[2] if len(sys.argv) > 2 else "compressible"
    if mode not in ("compressible", "random"):
        sys.exit(f"unknown mode {mode!r}: expected 'compressible' or 'random'")
    sys.exit(1 if main(sys.argv[1], mode) else 0)
