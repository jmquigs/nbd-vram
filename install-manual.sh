#!/bin/bash
# install-manual.sh - Minimal install of nbd-vram VRAM swap as a system service.
#
# Differences from install.sh:
#   - never invokes nvidia-smi and never sets any vm.* sysctl
#   - no battery / AC power management units, timer, or udev rule
#   - vram-swap-nbd.service is NOT enabled at boot: start it manually with
#       sudo systemctl start vram-swap-nbd
#   - the suspend hook only tears swap down before sleep; it does not restart
#     the daemon on resume
#   - VRAM_SETUP_SIZE_MB and VRAM_DISK_SIZE_MB are taken from the command line
#     and hardcoded into the installed unit. Re-run this script to change them.
#
# Usage: sudo ./install-manual.sh <VRAM_SETUP_SIZE_MB> <VRAM_DISK_SIZE_MB>
#   VRAM_SETUP_SIZE_MB  VRAM to allocate for swap, in MiB (minimum 1024)
#   VRAM_DISK_SIZE_MB   size of the swap device the kernel sees, in MiB. Pages
#                       are compressed before they reach VRAM, so this is
#                       normally larger than VRAM_SETUP_SIZE_MB (2x is
#                       conservative for zstd on anonymous pages).

set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    echo "usage: sudo $0 <VRAM_SETUP_SIZE_MB> <VRAM_DISK_SIZE_MB>" >&2
    echo "  VRAM_SETUP_SIZE_MB  VRAM to allocate for swap, in MiB (minimum 1024)" >&2
    echo "  VRAM_DISK_SIZE_MB   swap device size the kernel sees, in MiB (>= VRAM_SETUP_SIZE_MB)" >&2
    exit 1
}

is_uint() { case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }

[ $# -eq 2 ] || usage
SETUP_MB="$1"
DISK_MB="$2"
is_uint "$SETUP_MB" || { echo "error: VRAM_SETUP_SIZE_MB must be a whole number" >&2; usage; }
is_uint "$DISK_MB"  || { echo "error: VRAM_DISK_SIZE_MB must be a whole number" >&2; usage; }
[ "$SETUP_MB" -ge 1024 ]      || { echo "error: VRAM_SETUP_SIZE_MB too small (minimum 1024 MiB)" >&2; exit 1; }
[ "$DISK_MB" -ge "$SETUP_MB" ] || { echo "error: VRAM_DISK_SIZE_MB must be >= VRAM_SETUP_SIZE_MB" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "error: must run as root" >&2; exit 1; }

echo "=== nbd-vram installer (manual variant) ==="
echo "Source: $SRC_DIR"
echo "VRAM_SETUP_SIZE_MB=${SETUP_MB}  VRAM_DISK_SIZE_MB=${DISK_MB}"

# Stop a running instance cleanly via systemd first so ExecStop runs the safe
# swapoff before we replace the binary - never pkill a swap-backing daemon out
# from under active swap.
if systemctl is-active --quiet vram-swap-nbd.service 2>/dev/null; then
    echo "[pre] vram-swap-nbd.service is running - stopping for upgrade..."
    systemctl stop vram-swap-nbd.service || true
    sleep 1
fi

# Mop up any stray (non-systemd) test instance still holding VRAM
if pgrep -x nbd-vram &>/dev/null; then
    echo "[pre] stopping stray nbd-vram instance..."
    bash "$SRC_DIR/nbd-vram-disconnect.sh" 2>/dev/null || true
    pkill -x nbd-vram 2>/dev/null || true
    sleep 1
fi

# Remove power management from a previous full install.sh, if any
echo "[pre] removing power-management units from any previous full install..."
systemctl disable --now nbd-vram-battery-watch.timer   2>/dev/null || true
systemctl disable --now nbd-vram-battery-watch.service 2>/dev/null || true
systemctl disable --now nbd-vram-power-check.service   2>/dev/null || true
rm -f /etc/systemd/system/nbd-vram-battery-watch.timer
rm -f /etc/systemd/system/nbd-vram-battery-watch.service
rm -f /etc/systemd/system/nbd-vram-power-check.service
rm -f /etc/udev/rules.d/99-nbd-vram-power.rules
rm -f /usr/local/bin/nbd-vram-power-check.sh
rm -f /usr/local/bin/nbd-vram-sleep.sh
rm -f /run/nbd-vram-power-disabled /run/nbd-vram-sleep-disabled
# Disable old P2P-based services if present
systemctl disable --now vram-setup.service  2>/dev/null || true
systemctl disable --now vram-swapon.service 2>/dev/null || true
systemctl disable --now vram-swap2.service  2>/dev/null || true

# Ensure nbd-client is installed
echo "[1/4] Checking dependencies..."
if ! command -v nbd-client &>/dev/null; then
    echo "      installing nbd-client..."
    apt-get install -y nbd-client
fi
echo "      OK"

# Build the daemon
echo "[2/4] Building nbd-vram daemon..."
gcc -O2 -Wall -o "$SRC_DIR/nbd-vram" "$SRC_DIR/nbd-vram.c" -ldl -lpthread
echo "      OK"

# Install binary, scripts and units
echo "[3/4] Installing binaries and systemd units..."
UNIT=/etc/systemd/system/vram-swap-nbd.service
install -m 755 "$SRC_DIR/nbd-vram"                                  /usr/local/bin/nbd-vram
install -m 755 "$SRC_DIR/nbd-vram-connect.sh"                       /usr/local/bin/nbd-vram-connect.sh
install -m 755 "$SRC_DIR/nbd-vram-disconnect.sh"                    /usr/local/bin/nbd-vram-disconnect.sh
install -m 755 "$SRC_DIR/nbd-vram-sleep-manual.sh"                  /usr/local/bin/nbd-vram-sleep-manual.sh
install -m 644 "$SRC_DIR/systemd/manual/vram-swap-nbd.service"         "$UNIT"
install -m 644 "$SRC_DIR/systemd/manual/vram-swap-nbd-suspend.service" /etc/systemd/system/

# Hardcode sizes from the arguments and thread/connection count from nproc
NCPU=$(nproc)
sed -i "s/^Environment=VRAM_SETUP_SIZE_MB=.*/Environment=VRAM_SETUP_SIZE_MB=${SETUP_MB}/" "$UNIT"
sed -i "s/^Environment=VRAM_DISK_SIZE_MB=.*/Environment=VRAM_DISK_SIZE_MB=${DISK_MB}/"    "$UNIT"
sed -i "s/^Environment=VRAM_NBD_THREADS=.*/Environment=VRAM_NBD_THREADS=${NCPU}/"          "$UNIT"
sed -i "s/^Environment=VRAM_NBD_CONNECTIONS=.*/Environment=VRAM_NBD_CONNECTIONS=${NCPU}/"  "$UNIT"
echo "      VRAM_SETUP_SIZE_MB=${SETUP_MB} VRAM_DISK_SIZE_MB=${DISK_MB} threads/connections=${NCPU}"
echo "      OK"

echo "[4/4] Configuring services..."
systemctl daemon-reload
# A previous full install may have enabled the swap service at boot; undo that.
# The manual unit has no [Install] section, so it cannot be enabled again.
systemctl disable vram-swap-nbd.service 2>/dev/null || true
# The suspend hook must be enabled so it is pulled in by sleep.target.
systemctl enable vram-swap-nbd-suspend.service
echo "      OK"

echo ""
echo "=== Installation complete ==="
echo ""
echo "vram-swap-nbd.service is installed but NOT started or enabled at boot."
echo ""
echo "To start / stop:"
echo "  sudo systemctl start vram-swap-nbd"
echo "  sudo systemctl stop  vram-swap-nbd"
echo ""
echo "To check status:"
echo "  systemctl status vram-swap-nbd"
echo "  swapon --show"
echo "  journalctl -u vram-swap-nbd -n 20"
echo ""
echo "To change VRAM_SETUP_SIZE_MB / VRAM_DISK_SIZE_MB, re-run this script."
echo ""
echo "To uninstall:"
echo "  sudo bash uninstall-manual.sh"
