#!/bin/sh
# nbd-vram-sleep-manual.sh - tear down VRAM swap before sleep. Does NOT restore it.
# Driven by the manual-variant vram-swap-nbd-suspend.service: ExecStart calls 'pre'.
#
# Suspend powers down the GPU and destroys the CUDA context backing the NBD device.
# Swap I/O in flight against a dead context blocks forever and deadlocks resume
# (issue #19). Stopping the service runs the safe swapoff + disconnect + daemon exit
# (freeing all VRAM) while the GPU is still alive. The service's own stop path
# applies (swapoff-fails-aborts-disconnect, long TimeoutStopSec, no SIGKILL).
#
# After resume the service stays stopped; start it again manually when wanted.

case "$1" in
    pre)
        if systemctl is-active --quiet vram-swap-nbd.service; then
            echo "nbd-vram-sleep-manual: stopping VRAM swap before sleep" >&2
            # Blocks until swapoff completes and VRAM is freed, so the GPU suspends clean.
            systemctl stop vram-swap-nbd.service
        fi
        ;;
    *)
        echo "usage: $0 pre" >&2
        exit 1
        ;;
esac
