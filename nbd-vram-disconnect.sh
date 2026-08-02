#!/bin/bash
# nbd-vram-disconnect.sh - deactivate swap and disconnect nbd device
# Called by vram-swap-nbd.service ExecStop.
NBD_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || echo "/dev/nbd0")

# If swapoff fails (ENOMEM - pages can't migrate to other swap), do NOT disconnect.
# Disconnecting NBD with active swap pages mapped causes a kernel panic.
if ! swapoff "$NBD_DEV" 2>&1; then
    echo "nbd-vram-disconnect: swapoff $NBD_DEV FAILED - aborting disconnect to prevent panic" >&2
    exit 1
fi

nbd-client -d "$NBD_DEV" 2>/dev/null || true
rm -f /run/nbd-vram-dev

sysctl -w vm.swappiness=60 vm.min_free_kbytes=131072 2>/dev/null || true

echo "nbd-vram-disconnect: $NBD_DEV released"
