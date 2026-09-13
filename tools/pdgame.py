#!/usr/bin/env python3
"""Test input injection inside a GAME rather than the System launcher.

The launcher ignored injected buttons. That matches how the Playdate treats
System apps elsewhere (`eval` is blocked there too), so the real test is inside
a running game.

Each measurement is compared against an idle control taken in the same game, so
self-animation cannot be mistaken for a response.
"""
import os, sys, termios, time, select, zlib, struct, math

W, H, ROW = 400, 240, 50
port = sys.argv[1]
game = sys.argv[2] if len(sys.argv) > 2 else "/Games/PourOver.pdx"


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


def grab(fd):
    for _ in range(3):
        termios.tcflush(fd, termios.TCIFLUSH)
        os.write(fd, b"screen\r\n")
        d = read_for(fd, 1.2)
        m = d.find(b'~screen:\n')
        if m >= 0:
            p = d[m + 9:]
            if len(p) >= 12000:
                return p[:12000]
    return None


def pct(a, b):
    if not a or not b: return None
    return 100.0 * sum(bin(x ^ y).count('1') for x, y in zip(a, b)) / (12000 * 8)


def save_png(fb, path, vscale=2):
    rows = []
    for y in range(H):
        line = bytearray()
        for x in range(W):
            bit = (fb[y * ROW + (x >> 3)] >> (7 - (x & 7))) & 1
            line.append(255 if bit else 0)
        for _ in range(vscale):
            rows.append(b'\x00' + bytes(line))
    raw = b''.join(rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H * vscale, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


fd = open_port(port)
try:
    os.write(fd, b"echo off\r\n"); time.sleep(0.2); read_for(fd, 0.3)

    print("launching %s ..." % game)
    os.write(fd, ("run %s\r\n" % game).encode())
    time.sleep(6.0); read_for(fd, 0.5)

    g0 = grab(fd)
    if not g0:
        print("could not grab a screen after launch"); sys.exit(1)
    save_png(g0, 'game_launched.png')
    print("launched, screen captured -> game_launched.png")

    # Idle control, same duration as each test below.
    time.sleep(2.0)
    g1 = grab(fd)
    idle = pct(g0, g1)
    print("\nidle drift in-game over 2s : %.2f%%" % idle)
    thresh = max(idle * 3.0, 1.5)

    # --- buttons ---
    base = grab(fd)
    for _ in range(4):
        os.write(fd, b"btn +a\r\n"); time.sleep(0.12)
        os.write(fd, b"btn -a\r\n"); time.sleep(0.4)
    time.sleep(0.6)
    after = grab(fd)
    d_btn = pct(base, after)
    print("btn +a/-a x4               : %.2f%%   %s"
          % (d_btn, "RESPONDS" if d_btn and d_btn > thresh else "within noise"))
    if after: save_png(after, 'game_after_btn.png')

    # --- d-pad ---
    base = grab(fd)
    for _ in range(4):
        os.write(fd, b"btn +down\r\n"); time.sleep(0.12)
        os.write(fd, b"btn -down\r\n"); time.sleep(0.4)
    time.sleep(0.6)
    after = grab(fd)
    d_dpad = pct(base, after)
    print("btn +down/-down x4         : %.2f%%   %s"
          % (d_dpad, "RESPONDS" if d_dpad and d_dpad > thresh else "within noise"))

    # --- crank (known-good in Panic's cabinets) ---
    base = grab(fd)
    for i in range(12):
        os.write(fd, b"changecrank 30\r\n"); time.sleep(0.08)
    time.sleep(0.8)
    after = grab(fd)
    d_crank = pct(base, after)
    print("changecrank 30 x12         : %.2f%%   %s"
          % (d_crank, "RESPONDS" if d_crank and d_crank > thresh else "within noise"))

    # --- accel: hold one extreme, then the opposite ---
    for _ in range(10):
        os.write(fd, b"accel 1000 0 0\r\n"); time.sleep(0.05)
    time.sleep(0.8)
    ax = grab(fd)
    for _ in range(10):
        os.write(fd, b"accel -1000 0 0\r\n"); time.sleep(0.05)
    time.sleep(0.8)
    ay = grab(fd)
    d_accel = pct(ax, ay)
    print("accel +1g X vs -1g X       : %.2f%%   %s"
          % (d_accel, "RESPONDS" if d_accel and d_accel > thresh else "within noise"))
    if ax: save_png(ax, 'game_accel_plus.png')
    if ay: save_png(ay, 'game_accel_minus.png')

    print("\n(threshold for 'responds' was %.2f%%, i.e. 3x idle drift)" % thresh)

    # Return to the launcher so the device is left as we found it.
    os.write(fd, b"btn +b\r\n"); time.sleep(0.1); os.write(fd, b"btn -b\r\n")
finally:
    os.close(fd)
