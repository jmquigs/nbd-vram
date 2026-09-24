#!/bin/bash
# uninstall-manual.sh - Remove an nbd-vram install made by install-manual.sh

set -e

echo "=== nbd-vram uninstaller (manual variant) ==="

echo "[1/3] Stopping and disabling services..."
systemctl disable --now vram-swap-nbd-suspend.service 2>/dev/null || true
systemctl stop vram-swap-nbd.service 2>/dev/null || true
echo "      OK"

echo "[2/3] Removing binaries and systemd units..."
rm -f /usr/local/bin/nbd-vram
rm -f /usr/local/bin/nbd-vram-connect.sh
rm -f /usr/local/bin/nbd-vram-disconnect.sh
rm -f /usr/local/bin/nbd-vram-sleep-manual.sh
rm -f /etc/systemd/system/vram-swap-nbd.service
rm -f /etc/systemd/system/vram-swap-nbd-suspend.service
echo "      OK"

echo "[3/3] Reloading systemd..."
systemctl daemon-reload
echo "      OK"

echo ""
echo "=== Uninstall complete ==="
