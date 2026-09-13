#!/usr/bin/env python3
"""Dry-run the firmware's exact connect sequence against a real Playdate.

The host tests prove the parser handles synthetic data. They cannot prove the
device actually answers the way the firmware expects. This replays the precise
command order from pd_stream_on_connect() and checks the device's real replies,
so a flash is not the first time that sequence meets hardware.

    python3 tools/dryrun.py /dev/cu.usbmodemPD...
"""
import os, sys, termios, time, select

W, H, ROW = 400, 240, 50
port = sys.argv[1]

SCREEN_MARKER = b"~screen:\n"
SCREEN_BYTES = 12000


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(a[6]); cc[termios.VMIN] = 0; cc[termios.VTIME] = 0; a[6] = cc
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_for(fd, s):
    out = bytearray(); end = time.time() + s
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                c = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if c: out += c
    return bytes(out)


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


ok = 0
fail = 0


def check(cond, what, detail=""):
    global ok, fail
    if cond:
        ok += 1; print("  ok   %s %s" % (what, detail))
    else:
        fail += 1; print("  FAIL %s %s" % (what, detail))


fd = open_port(port)
try:
    print("Replaying pd_stream_on_connect() exactly as the firmware sends it.\n")

    # 1. echo off
    os.write(fd, b"echo off\r\n"); time.sleep(0.2)
    read_for(fd, 0.3)
    termios.tcflush(fd, termios.TCIFLUSH)

    # 2. autolock onBattery
    os.write(fd, b"autolock onBattery\r\n"); time.sleep(0.2)
    r = read_for(fd, 0.4)
    check(b"rror" not in r and b"nknown" not in r, "autolock onBattery accepted")

    # 3. stream a-  (audio off)
    os.write(fd, b"stream a-\r\n"); time.sleep(0.2)
    r = read_for(fd, 0.4)
    check(b"rror" not in r and b"nknown" not in r, "stream a- accepted")

    # 4. screen -> marker then exactly 12,000 bytes
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, b"screen\r\n")
    d = read_for(fd, 2.0)
    m = d.find(SCREEN_MARKER)
    check(m >= 0, "screen reply carries the ~screen:\\n marker")
    payload = d[m + len(SCREEN_MARKER):] if m >= 0 else b""
    check(len(payload) >= SCREEN_BYTES, "12,000 bytes of framebuffer arrived",
          "(got %d)" % len(payload))

    fb = payload[:SCREEN_BYTES]
    nonzero = sum(1 for b in fb if b)
    check(0 < nonzero < SCREEN_BYTES, "framebuffer looks like a picture, not all one value",
          "(%d/%d non-zero bytes)" % (nonzero, SCREEN_BYTES))

    # 5. stream enable, then verify real frames parse
    os.write(fd, b"stream enable\r\n"); time.sleep(0.3)
    buf = bytearray()
    frames = rows = bad = 0
    end = time.time() + 3.0
    next_poke = 0
    total = 0
    while time.time() < end:
        c = read_for(fd, 0.05)
        if c:
            total += len(c); buf += c
        while len(buf) >= 4:
            op, arg = buf[0], buf[1]
            sz = buf[2] | (buf[3] << 8)
            if not header_valid(op, arg, sz):
                bad += 1; del buf[0]; continue
            if len(buf) < 4 + sz:
                break
            p = bytes(buf[4:4 + sz]); del buf[:4 + sz]
            if op == 12:
                if 1 <= rev8(p[0]) <= H: rows += 1
            elif op == 11:
                frames += 1
        now = time.time()
        if now >= next_poke:
            os.write(fd, b"stream poke\r\n"); next_poke = now + 0.3

    check(frames > 10, "frames arriving after stream enable", "(%d frames in 3s)" % frames)
    check(rows > 0, "row messages parsed", "(%d rows)" % rows)
    check(bad < 200, "resync churn stays low with the tightened validator",
          "(%d bad header bytes)" % bad)
    print("\n  throughput: %.0f KB/s, %.1f fps" % (total / 3.0 / 1024, frames / 3.0))

    # 6. accel at the frame-sync rate the firmware will use
    sent = 0
    f0 = frames
    end = time.time() + 2.0
    next_poke = 0
    while time.time() < end:
        c = read_for(fd, 0.03)
        if c:
            buf += c
            while len(buf) >= 4:
                op, arg = buf[0], buf[1]
                sz = buf[2] | (buf[3] << 8)
                if not header_valid(op, arg, sz):
                    del buf[0]; continue
                if len(buf) < 4 + sz:
                    break
                del buf[:4 + sz]
                if op == 11: frames += 1
        os.write(fd, b"accel 0 1000 0\r\n"); sent += 1
        now = time.time()
        if now >= next_poke:
            os.write(fd, b"stream poke\r\n"); next_poke = now + 0.3

    check(frames - f0 > 5, "stream keeps delivering while accel is injected",
          "(%d frames, %d accel sent)" % (frames - f0, sent))

    # 7. graceful teardown, as pd_stream_on_disconnect() does
    os.write(fd, b"stream disable\r\n"); time.sleep(0.3)
    read_for(fd, 0.5)
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, b"version\r\n")
    r = read_for(fd, 1.0)
    check(len(r) > 0, "console still responsive after a clean teardown")
    print("\n  device version reply: %s" % r.decode('utf-8', 'replace').strip()[:120])
finally:
    os.close(fd)

print("\n===== %d ok, %d failed =====" % (ok, fail))
sys.exit(1 if fail else 0)
