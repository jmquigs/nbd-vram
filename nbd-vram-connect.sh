#!/bin/bash
# nbd-vram-connect.sh - find a free nbd device, connect, activate swap
# Called by vram-swap-nbd.service ExecStartPost after nbd-vram signals ready.
set -e

modprobe nbd max_part=0 2>/dev/null || true

# Clean up any device left over from a previous unclean shutdown
if [ -f /run/nbd-vram-dev ]; then
    PREV=$(cat /run/nbd-vram-dev)
    swapoff "$PREV" 2>/dev/null || true
    nbd-client -d "$PREV" 2>/dev/null || true
    rm -f /run/nbd-vram-dev
fi

# Find first free /dev/nbdN by checking kernel's pid file for each device
NBD_DEV=""
for dev in /dev/nbd{0..15}; do
    [ -b "$dev" ] || continue
    pid=$(cat "/sys/block/$(basename "$dev")/pid" 2>/dev/null || true)
    if [ -z "$pid" ]; then
        NBD_DEV="$dev"
        break
    fi
done

if [ -z "$NBD_DEV" ]; then
    echo "nbd-vram-connect: no free nbd device found (nbd0-nbd15 all in use)" >&2
    exit 1
fi

echo "nbd-vram-connect: using $NBD_DEV"
# Force-disconnect to clear any stale kernel state from an unclean previous shutdown
nbd-client -d "$NBD_DEV" 2>/dev/null || true
nbd-client -unix /run/nbd-vram.sock "$NBD_DEV" -connections ${VRAM_NBD_CONNECTIONS:-4}
mkswap "$NBD_DEV"
# --discard=pages returns freed swap slots to the daemon as TRIMs, which is what
# releases their VRAM. Without it the compressed store only ever grows, and an
# overcommitted device fills far sooner than it needs to.
swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}" --discard=pages 2>/dev/null \
    || swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}"

# Save device name so disconnect script knows what to clean up
echo "$NBD_DEV" > /run/nbd-vram-dev
echo "nbd-vram-connect: swap active on $NBD_DEV"
