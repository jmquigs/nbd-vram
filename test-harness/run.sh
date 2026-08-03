#!/bin/bash
# run.sh - exercise nbd-vram's compressed storage layer without a GPU.
#
# Builds a stub libcuda.so.1 that backs "VRAM" with host memory, builds the real
# daemon, points it at the stub with LD_LIBRARY_PATH, and drives it over NBD.
# The binary under test is the one that ships - it has no test hooks.
#
#   ./run.sh                       functional + concurrency suite
#   ./run.sh --codec lz4           ... against a different codec (zstd|lz4|none)
#   ./run.sh --capacity random     fill the device; incompressible data hits ENOSPC
#   ./run.sh --capacity compressible
#   ./run.sh --fragment            fill/drain/refill cycles; measures occupancy
#   ./run.sh --tsan                functional suite under ThreadSanitizer
#   ./run.sh --all                 everything above, in sequence
#
# SRC=/path/to/other.c ./run.sh --fragment  builds a different source, for A/B
# measurement of an allocator change against its own baseline.
set -u

cd "$(dirname "$0")"
SRC=${SRC:-../nbd-vram.c}
SOCK=$PWD/t.sock

# The daemon's allocation loop is `while (mb >= 1024)`, so anything below 1024
# exits with "all allocation attempts failed" and looks like a harness bug.
VRAM_MB=1024
DISK_MB=2048
THREADS=8      # one worker serves one connection; the suite opens 5

CODEC=zstd
MODE=functional
TSAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --codec)    CODEC="$2"; shift 2 ;;
        --capacity) MODE=capacity; FILL="${2:-compressible}"; shift 2 ;;
        --fragment) MODE=fragment; shift ;;
        --tsan)     TSAN=1; shift ;;
        --all)      MODE=all; shift ;;
        -h|--help)  sed -n '2,14p' "$0"; exit 0 ;;
        *)          echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Always kill by exact name. `pkill -f nbd-vram` also matches this script's own
# command line and kills the shell running it, which surfaces as a bare exit 144
# with no output and looks exactly like the daemon crashing.
cleanup() { pkill -x nbd-vram >/dev/null 2>&1; pkill -x nbd-vram-tsan >/dev/null 2>&1; rm -f "$SOCK"; }
trap cleanup EXIT

build() {
    gcc -O1 -shared -fPIC -o libcuda.so.1 fakecuda.c || return 1
    if [ "$TSAN" = 1 ]; then
        gcc -O1 -g -fsanitize=thread -o nbd-vram-tsan "$SRC" -ldl -lpthread || return 1
        DAEMON=./nbd-vram-tsan
    else
        gcc -O2 -Wall -Wextra -o nbd-vram "$SRC" -ldl -lpthread || return 1
        DAEMON=./nbd-vram
    fi
}

# Every run gets a brand new daemon. Reusing one across runs leaves blocks
# written, and the "unwritten blocks read as zeros" assertions fail for reasons
# that have nothing to do with the code under test.
start_daemon() {
    cleanup; sleep 0.3
    : > daemon.log
    LD_LIBRARY_PATH=$PWD \
    TSAN_OPTIONS="halt_on_error=0 suppressions=$PWD/tsan.supp" \
    VRAM_NO_MLOCK=1 \
    VRAM_SOCK_PATH="$SOCK" \
    VRAM_SETUP_SIZE_MB=$VRAM_MB \
    VRAM_DISK_SIZE_MB=$DISK_MB \
    VRAM_NBD_THREADS=$THREADS \
    VRAM_COMPRESS="$CODEC" \
    VRAM_ALLOC_BINS="${VRAM_ALLOC_BINS:-4}" \
    VRAM_STATS_INTERVAL_SEC="${1:-0}" \
        $DAEMON > daemon.log 2>&1 &
    for _ in $(seq 1 40); do [ -S "$SOCK" ] && return 0; sleep 0.25; done
    echo "daemon failed to start:" >&2; cat daemon.log >&2; return 1
}

rc=0

run_functional() {
    echo "=== functional + concurrency (codec=$CODEC${1:+, $1}) ==="
    start_daemon 0 || return 1
    grep -E "compression|logical over" daemon.log
    python3 -u nbdtest.py "$SOCK" $THREADS || rc=1
}

run_capacity() {
    echo "=== capacity: $1 data (codec=$CODEC) ==="
    start_daemon "${FRAG_STATS_SEC:-5}" || return 1
    python3 -u capacity.py "$SOCK" "$1" || rc=1
    sleep 6   # let one stats line land
    grep -E "stats:|heap .*committed|full" daemon.log | tail -3
}

# fragment.py reads daemon.log to get the occupancy the daemon reports, so the
# stats interval has to be short enough that a fresh line lands after the
# workload rather than only at shutdown.
run_fragment() {
    echo "=== fragmentation: fill/drain/refill occupancy (codec=$CODEC) ==="
    start_daemon "${FRAG_STATS_SEC:-5}" || return 1
    python3 -u fragment.py "$SOCK" daemon.log || rc=1
    grep -E "stats bins:" daemon.log | tail -1
}

case "$MODE" in
    functional) build || exit 1; run_functional "${TSAN:+tsan}" ;;
    capacity)   build || exit 1; run_capacity "$FILL" ;;
    fragment)   build || exit 1; run_fragment ;;
    all)
        TSAN=0; build || exit 1
        for CODEC in zstd lz4 none; do run_functional; echo; done
        CODEC=zstd
        run_capacity compressible; echo
        run_capacity random; echo
        run_fragment; echo
        TSAN=1; build || exit 1
        run_functional "ThreadSanitizer"
        ;;
esac

if [ "$TSAN" = 1 ]; then
    # grep -c prints 0 but exits 1 on no matches, so swallow the status rather
    # than appending a second count with `|| echo 0`.
    n=$(grep -c "WARNING: ThreadSanitizer" daemon.log 2>/dev/null || true)
    [ -z "$n" ] && n=0
    echo "ThreadSanitizer warnings: $n"
    if [ "$n" != 0 ]; then grep "^SUMMARY" daemon.log | sort -u; rc=1; fi
fi

echo
[ $rc = 0 ] && echo "run.sh: OK" || echo "run.sh: FAILURES (see above)"
exit $rc
