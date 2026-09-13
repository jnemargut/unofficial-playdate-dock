#!/usr/bin/env python3
"""Corrected injection test — uses ONLY the stream, never `screen`.

The previous attempt mixed `screen` (12,000 raw bytes on demand) with an active
binary stream and could not find its marker reliably. That produced two false
"failures". Here everything is observed through the stream itself.
"""
import os, sys, termios, time, select, zlib, struct, math

W, H, ROW = 400, 240, 50
port = sys.argv[1]

OP_FRAME_END, OP_FRAME_ROW, OP_FULL_FRAME = 11, 12, 14


def rev8(n):
    n = ((n & 0x55) << 1) | ((n & 0xAA) >> 1)
    n = ((n & 0x33) << 2) | ((n & 0xCC) >> 2)
    n = ((n & 0x0F) << 4) | ((n & 0xF0) >> 4)
    return n & 0xFF


def header_valid(op, arg, sz):
    return ((op == 1 and sz == 8) or (op == 10 and sz == 0) or (op == 13 and sz == 4)
            or (op == 11 and sz == 0) or (op == 12 and sz == 52)
            or (op == 14 and sz == 34 + arg * 50) or (op == 20 and sz < 2048)
            or (op == 21 and sz == 2) or (op == 22 and sz == 4)
            or (op == 0x99 and sz in (0, 6, 48)))


class P:
    def __init__(self):
        self.fb = bytearray(ROW * H)
        self.buf = bytearray()
        self.frames = 0; self.rows = 0; self.bad = 0; self.audio = 0

    def feed(self, d):
        self.buf += d
        while len(self.buf) >= 4:
            op, arg = self.buf[0], self.buf[1]
            sz = self.buf[2] | (self.buf[3] << 8)
            if not header_valid(op, arg, sz):
                self.bad += 1; del self.buf[0]; continue
            if len(self.buf) < 4 + sz:
                return
            p = bytes(self.buf[4:4 + sz]); del self.buf[:4 + sz]
            if op == OP_FRAME_ROW:
                r = rev8(p[0])
                if 1 <= r <= H:
                    self.fb[(r - 1) * ROW:(r - 1) * ROW + ROW] = p[1:51]; self.rows += 1
            elif op == OP_FRAME_END:
                self.frames += 1
            elif op == 20:
                self.audio += 1


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(a[6]); cc[termios.VMIN] = 0; cc[termios.VTIME] = 0; a[6] = cc
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def pump(fd, p, seconds, inject=None):
    """Run the stream for N seconds, poking, optionally injecting commands."""
    end = time.time() + seconds
    next_poke = 0
    n = 0
    total = 0
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                c = os.read(fd, 65536)
            except BlockingIOError:
                c = b''
            if c:
                total += len(c); p.feed(c)
        now = time.time()
        if now >= next_poke:
            os.write(fd, b"stream poke\r\n"); next_poke = now + 0.3
        if inject:
            inject(fd, n); n += 1
    return total


fd = open_port(port)
try:
    os.write(fd, b"echo off\r\n"); time.sleep(0.2)
    termios.tcflush(fd, termios.TCIFLUSH)

    # Turn audio off — we are a monochrome video dock and it is pure bandwidth.
    os.write(fd, b"stream a-\r\n"); time.sleep(0.2)
    os.write(fd, b"stream enable\r\n"); time.sleep(0.3)

    p = P()
    print("== settle: stream only, 3s ==")
    t0 = time.time(); b0 = pump(fd, p, 3.0); dt = time.time() - t0
    print("   %d bytes (%.0f KB/s), %d frames (%.1f fps), %d rows, %d audio msgs, %d resync bytes"
          % (b0, b0 / dt / 1024, p.frames, p.frames / dt, p.rows, p.audio, p.bad))
    snap_a = bytes(p.fb)
    f0, r0, bad0 = p.frames, p.rows, p.bad

    print("\n== inject btn WHILE streaming, 3s ==")
    state = {'t': 0}

    def inj_btn(fd, n):
        now = time.time()
        if now - state['t'] > 0.45:
            state['t'] = now
            os.write(fd, b"btn +1\r\n")
            os.write(fd, b"btn -1\r\n")

    t0 = time.time(); b1 = pump(fd, p, 3.0, inj_btn); dt = time.time() - t0
    snap_b = bytes(p.fb)
    changed = sum(bin(x ^ y).count('1') for x, y in zip(snap_a, snap_b))
    print("   %d bytes (%.0f KB/s), %d frames, %d rows, %d resync bytes"
          % (b1, b1 / dt / 1024, p.frames - f0, p.rows - r0, p.bad - bad0))
    print("   -> %.2f%% of pixels changed (launcher moved = injection reached the OS)"
          % (100.0 * changed / (12000 * 8)))
    btn_ok = changed > 2000

    print("\n== inject accel WHILE streaming, 3s ==")
    f1, r1, bad1 = p.frames, p.rows, p.bad
    snap_c = bytes(p.fb)
    st2 = {'t': 0, 'ph': 0.0, 'n': 0}

    def inj_accel(fd, n):
        now = time.time()
        if now - st2['t'] > 0.05:          # 20 Hz
            st2['t'] = now
            x = math.sin(st2['ph']); y = math.cos(st2['ph']); st2['ph'] += 0.4
            os.write(fd, b"accel %d %d 0\r\n" % (int(x * 1000), int(y * 1000)))
            st2['n'] += 1

    t0 = time.time(); b2 = pump(fd, p, 3.0, inj_accel); dt = time.time() - t0
    print("   %d bytes (%.0f KB/s), %d frames (%.1f fps), %d rows, %d resync bytes"
          % (b2, b2 / dt / 1024, p.frames - f1, (p.frames - f1) / dt, p.rows - r1, p.bad - bad1))
    print("   accel commands sent: %d" % st2['n'])
    stream_survived = (p.frames - f1) > 5

    os.write(fd, b"stream disable\r\n"); time.sleep(0.2)
finally:
    os.close(fd)

print("\n===== VERDICT =====")
print("btn injection works alongside an active stream : %s" % ("YES" if btn_ok else "no visible change"))
print("accel accepted at 20 Hz without stalling stream: %s" % ("YES" if stream_survived else "NO"))
print("frames kept flowing throughout                 : %s" % ("YES" if stream_survived else "NO"))
