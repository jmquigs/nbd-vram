SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
gcc -O2 -Wall -o "$SRC_DIR/nbd-vram" "$SRC_DIR/nbd-vram.c" -ldl -lpthread
sudo install -m 755 "$SRC_DIR/nbd-vram" /usr/local/bin/nbd-vram
sudo install -o root -g root -m 755 nbd-vram-disconnect.sh /usr/local/sbin/nbd-vram-disconnect
sudo install -o root -g root -m 755 test-nbd.sh /usr/local/sbin/test-nbd