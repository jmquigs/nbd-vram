# Host-side test harness

Exercises nbd-vram's compressed storage layer — the block index, the extent
allocator, and the compression paths — on a machine with **no NVIDIA GPU and no
root**.

The repository's other tests (`test-nbd.sh`, `test-fill.sh`, `benchmarks/`) all
need real hardware and a live swap device. That leaves the storage layer's
concurrency paths effectively untestable, which is a problem, because a bug
there hands one process's page to another and the daemon's own comments explain
why that is fatal.

## How it works

`fakecuda.c` builds a stand-in `libcuda.so.1` where `cuMemAlloc` is `calloc`,
the async copies are `memcpy`, and stream synchronize is a no-op. The daemon
already loads CUDA with `dlopen("libcuda.so.1")`, so `LD_LIBRARY_PATH` is enough
to put the stub in front of it.

**The binary under test is the one that ships.** There are no test hooks, no
`#ifdef`s and no fake-device mode in `nbd-vram.c`. If it passes here, that same
build is what runs against a real GPU.

## Running it

```sh
./run.sh                        # functional + concurrency suite (10 groups)
./run.sh --codec lz4            # ... against another codec: zstd | lz4 | none
./run.sh --capacity compressible  # fill the device; should never run out
./run.sh --capacity random        # incompressible fill; should hit ENOSPC cleanly
./run.sh --tsan                 # functional suite under ThreadSanitizer
./run.sh --all                  # all of the above, in sequence (~4 minutes)
```

Needs only `gcc` and `python3`. `run.sh` builds both the stub and the daemon,
starts a fresh daemon, runs the tests, and tears everything down on exit.

## What is covered

| | |
|---|---|
| `nbdtest.py` | A minimal NBD fixed-newstyle client plus ten groups: unwritten blocks reading as zeros, aligned round trips, incompressible data taking the raw path, multi-block and oversized requests, misaligned read-modify-write, repeated overwrite of one block (slot displacement), TRIM reclaiming, out-of-bounds returning EINVAL without dropping the connection, four connections writing and verifying concurrently, and readers hammering one block under continuous rewrite. |
| `capacity.py` | Fills the whole device and reports the achieved ratio. In `random` mode it drives the heap to full and checks what matters about ENOSPC: that the connection stays up and that everything written beforehand still reads back byte-for-byte. |

Group 10 is the one worth keeping. It is the only thing that reaches the
lock-free read path's validate-after-DMA retry, because a real swap workload
essentially never issues a read and a write to the same page concurrently — the
page lock prevents it. Watch `retries` in the daemon's stats line go up during
that group; that is the mechanism firing.

## Gotchas

Each of these cost real debugging time and none is visible from reading the code.

- **Kill the daemon with `pkill -x nbd-vram`, never `pkill -f nbd-vram`.** The
  `-f` form matches the calling shell's own command line and kills the shell.
  It surfaces as a bare exit 144 with no output and looks exactly like the
  daemon crashing. `run.sh` always uses `-x`.
- **Every run needs a brand new daemon.** Reusing one leaves blocks written, and
  the "unwritten blocks read as zeros" assertions then fail for reasons that
  have nothing to do with the code under test.
- **One worker serves one connection.** Opening more connections than
  `VRAM_NBD_THREADS` leaves the extras blocked in the handshake forever, which
  looks like a daemon deadlock. `nbdtest.py` takes the worker count and refuses
  to start rather than hanging.
- **`VRAM_SETUP_SIZE_MB` below 1024 fails outright** — the allocation backoff
  loop in `nbd-vram.c` is `while (mb >= 1024)`, so anything smaller exits with
  "all allocation attempts failed". Pre-existing, but it reads as a harness bug.
- **`VRAM_NO_MLOCK=1` is required** when running unprivileged, and
  `PR_SET_IO_FLUSHER failed (Operation not permitted)` in the daemon log is
  expected and harmless in a container.

## Known behaviour, not bugs

- **An overwrite can still fail with ENOSPC on a completely full heap.** A write
  reserves its new slot before releasing the one it displaces, which is what
  lets a concurrent reader keep using the old slot safely. At 100% full there is
  nothing to reserve, so even a same-size overwrite is refused. `capacity.py`
  accepts this outcome explicitly.
- **`tsan.supp` suppresses two things**, both documented in the file: the stub
  device turning an intentional-and-validated stale read into a visible `memcpy`
  race, and the pre-existing `volatile`-instead-of-atomic flags in the SIGTERM
  drain path (`nbd-vram.c:700-725`, from the issue #19 work). Nothing in the
  index, allocator or codec paths is suppressed — those are clean and must stay
  clean.

## Limitations

The stub is not a GPU. It does not model DMA latency, PCIe bandwidth, stream
ordering, or any real CUDA failure mode, and `cuStreamSynchronize` returning
instantly means the harness cannot catch a missing synchronize. It tests the
storage logic, not the device integration — real-hardware verification still
belongs to `test-nbd.sh`, `test-fill.sh` and `benchmarks/`.

Ratios reported here are meaningless as predictions: `compressible()` generates
synthetic data far more compressible than real anonymous memory (it reports
~24x, where zstd level 1 on real anon pages lands nearer 2.5-3x). It is there to
exercise the code path, not to estimate capacity.
