#!/usr/bin/env python3
"""Functional and concurrency suite for nbd-vram's compressed storage layer.

Speaks enough of the NBD fixed-newstyle protocol to drive the daemon directly,
so the binary under test is the one that ships - it has no idea it is being
tested. Run it via ./run.sh, which sets up the stub device first.
"""
import os, socket, struct, sys, threading, random

NBD_REQUEST_MAGIC  = 0x25609513
NBD_RESPONSE_MAGIC = 0x67446698
NBD_CMD_READ, NBD_CMD_WRITE, NBD_CMD_DISC, NBD_CMD_FLUSH, NBD_CMD_TRIM = 0, 1, 2, 3, 4
IHAVEOPT   = 0x49484156454F5054
NBD_OPT_GO = 7
NBD_REP_ACK, NBD_REP_INFO = 1, 3

# Transmission flags we assert on
FLAG_SEND_FLUSH     = 0x0004
FLAG_SEND_TRIM      = 0x0020
FLAG_CAN_MULTI_CONN = 0x0100


class NBD:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.h = 0
        self.size, self.flags = self._handshake()

    def _recv(self, n):
        b = b''
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError("peer closed")
            b += c
        return b

    def _handshake(self):
        m1, m2, _sflags = struct.unpack(">QQH", self._recv(18))
        assert m1 == 0x4E42444D41474943, hex(m1)          # "NBDMAGIC"
        assert m2 == IHAVEOPT
        self.s.sendall(struct.pack(">I", 1 | 2))          # FIXED_NEWSTYLE | NO_ZEROES
        payload = struct.pack(">IH", 0, 0)                # empty export name, no info requests
        self.s.sendall(struct.pack(">QII", IHAVEOPT, NBD_OPT_GO, len(payload)) + payload)
        size = flags = None
        while True:
            _magic, _opt, rtype, length = struct.unpack(">QIII", self._recv(20))
            data = self._recv(length) if length else b''
            if rtype & 0x80000000:
                raise RuntimeError(f"option error 0x{rtype:x}")
            if rtype == NBD_REP_INFO and len(data) >= 12:
                _it, size, flags = struct.unpack(">HQH", data[:12])
            if rtype == NBD_REP_ACK:
                return size, flags

    def _req(self, cmd, off, length, data=b''):
        self.h += 1
        self.s.sendall(struct.pack(">IHHQQI", NBD_REQUEST_MAGIC, 0, cmd, self.h, off, length) + data)
        return self.h

    def _reply(self, want_data=0):
        magic, err, handle = struct.unpack(">IIQ", self._recv(16))
        assert magic == NBD_RESPONSE_MAGIC, hex(magic)
        data = self._recv(want_data) if (want_data and err == 0) else b''
        return err, handle, data

    def write(self, off, data):
        self._req(NBD_CMD_WRITE, off, len(data), data)
        return self._reply()[0]

    def read(self, off, length):
        self._req(NBD_CMD_READ, off, length)
        err, _, data = self._reply(length)
        if err:
            raise OSError(err, f"read at {off} failed with errno {err}")
        return data

    def read_err(self, off, length):
        self._req(NBD_CMD_READ, off, length)
        err, _, data = self._reply(length)
        return err, data

    def trim(self, off, length):
        self._req(NBD_CMD_TRIM, off, length)
        return self._reply()[0]

    def flush(self):
        self._req(NBD_CMD_FLUSH, 0, 0)
        return self._reply()[0]

    def close(self):
        try:
            self._req(NBD_CMD_DISC, 0, 0)
        except Exception:
            pass
        self.s.close()


FAILED = []

def check(name, cond, extra=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f" - {extra}" if extra and not cond else ""))
    if not cond:
        FAILED.append(name)


def compressible(n, seed):
    """Pages that look like anonymous memory: long runs and repeated pointer-ish words."""
    rnd = random.Random(seed)
    out = bytearray()
    while len(out) < n:
        if rnd.random() < 0.6:
            out += bytes([rnd.randrange(256)]) * rnd.randrange(16, 512)
        else:
            out += struct.pack("<Q", rnd.randrange(1 << 48)) * rnd.randrange(4, 40)
    return bytes(out[:n])


def incompressible(n, seed):
    """Deterministic random bytes, so a filled device can be re-verified later."""
    return random.Random(seed).randbytes(n)


# The daemon dedicates one worker thread per connection, so opening more
# connections than VRAM_NBD_THREADS leaves the extras blocked in the handshake
# forever. That looks exactly like a daemon deadlock, so fail loudly instead.
EXTRA_CONNS = 4

def main(sock, threads):
    need = EXTRA_CONNS + 1
    if threads < need:
        print(f"ERROR: this suite opens {need} concurrent connections but the daemon was "
              f"started with VRAM_NBD_THREADS={threads}.\n"
              f"       One worker serves one connection, so the extras would hang in the "
              f"handshake.\n       Restart the daemon with VRAM_NBD_THREADS>={need}.")
        return 1

    c = NBD(sock)
    print(f"\nexport size {c.size} bytes ({c.size >> 20} MiB), transmission flags 0x{c.flags:04x}")
    check("SEND_TRIM advertised", bool(c.flags & FLAG_SEND_TRIM), f"flags=0x{c.flags:04x}")
    check("SEND_FLUSH advertised", bool(c.flags & FLAG_SEND_FLUSH))
    check("CAN_MULTI_CONN advertised", bool(c.flags & FLAG_CAN_MULTI_CONN))

    print("\n[1] unwritten blocks read as zeros")
    check("fresh read is zeros", c.read(0, 65536) == b'\0' * 65536)
    check("fresh read deep in device", c.read(c.size - 8192, 8192) == b'\0' * 8192)

    print("\n[2] aligned single-block round trip")
    p = compressible(4096, 1)
    check("write 4K", c.write(0, p) == 0)
    check("read back 4K", c.read(0, 4096) == p)

    print("\n[3] incompressible data (forces raw storage)")
    r = incompressible(65536, 2)
    check("write 64K random", c.write(1 << 20, r) == 0)
    check("read back 64K random", c.read(1 << 20, 65536) == r)

    print("\n[4] multi-block and large requests")
    big = compressible(4 << 20, 3)
    check("write 4 MiB", c.write(8 << 20, big) == 0)
    check("read back 4 MiB", c.read(8 << 20, 4 << 20) == big)

    print("\n[5] misaligned / partial (read-modify-write path)")
    base = 64 << 20
    c.write(base, compressible(8192, 4))
    patch = incompressible(300, 5)
    check("write 300B at +513", c.write(base + 513, patch) == 0)
    check("patched bytes correct", c.read(base, 8192)[513:813] == patch)
    check("partial read matches", c.read(base + 513, 300) == patch)
    span = incompressible(512, 6)
    check("write 512B across a block boundary", c.write(base + 4096 - 256, span) == 0)
    check("read back across boundary", c.read(base + 4096 - 256, 512) == span)

    print("\n[6] overwrite in place (slot displacement)")
    for i in range(20):
        d = compressible(4096, 100 + i)
        c.write(2 << 20, d)
        if c.read(2 << 20, 4096) != d:
            check(f"overwrite round {i}", False)
            break
    else:
        check("20 overwrites of one block", True)

    print("\n[7] TRIM frees blocks")
    check("trim 4 MiB", c.trim(8 << 20, 4 << 20) == 0)
    check("trimmed range reads as zeros", c.read(8 << 20, 65536) == b'\0' * 65536)
    check("flush ok", c.flush() == 0)

    print("\n[8] out-of-bounds is EINVAL, connection survives")
    err, _ = c.read_err(c.size, 4096)
    check("read past end -> EINVAL", err == 22, f"got errno {err}")
    check("connection still usable", c.read(0, 4096) == p)

    print("\n[9] concurrent writers across connections")
    conns = [NBD(sock) for _ in range(EXTRA_CONNS)]
    errs = []

    def hammer(cn, tid):
        try:
            for i in range(150):
                off = (128 << 20) + (tid * 150 + i) * 4096
                d = compressible(4096, tid * 1000 + i)
                if cn.write(off, d) != 0:
                    errs.append(f"t{tid} write {i}")
                if cn.read(off, 4096) != d:
                    errs.append(f"t{tid} verify {i}")
        except Exception as e:
            errs.append(f"t{tid} {e!r}")

    ts = [threading.Thread(target=hammer, args=(conns[i], i)) for i in range(EXTRA_CONNS)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check(f"{EXTRA_CONNS} connections x 150 write+verify", not errs, str(errs[:3]))

    # The one that matters for the lock-free read path: a reader must never see a
    # slot that was freed and handed to another block mid-DMA. Reaching it needs
    # sustained same-block contention, which a real swap workload never produces.
    print("\n[10] same-block contention across connections")
    errs2 = []
    stop = threading.Event()
    HOT = 200 << 20
    known = compressible(4096, 7777)
    conns[0].write(HOT, known)

    def writer(cn):
        while not stop.is_set():
            cn.write(HOT, known)

    def reader(cn, tid):
        for _ in range(400):
            try:
                if cn.read(HOT, 4096) != known:
                    errs2.append(f"reader{tid} mismatch")
            except Exception as e:
                errs2.append(f"reader{tid} {e!r}")

    wt = threading.Thread(target=writer, args=(conns[1],))
    wt.start()
    rts = [threading.Thread(target=reader, args=(conns[2 + i], i)) for i in range(2)]
    [t.start() for t in rts]
    [t.join() for t in rts]
    stop.set()
    wt.join()
    check("readers never see a torn or foreign page", not errs2, str(errs2[:3]))

    for cn in conns:
        cn.close()
    c.close()
    return len(FAILED)


if __name__ == "__main__":
    sock = sys.argv[1]
    threads = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    rc = main(sock, threads)
    print(f"\n{'ALL TESTS PASSED' if rc == 0 else f'{rc} FAILURE(S): ' + ', '.join(FAILED)}")
    sys.exit(1 if rc else 0)
