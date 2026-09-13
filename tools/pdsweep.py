#!/usr/bin/env python3
"""Find a game that responds to injected accelerometer data.

btn injection is now proven to reach games (2.52% vs a 0.00% idle control in
PourOver). accel uses the same transport into a different Playdate API, so the
remaining question is only whether that API receives it — which needs a game
that actually reads tilt.

Each game gets its own idle control, so a self-animating title cannot be
mistaken for a response.
"""
import os, sys, termios, time, select, zlib, struct

W, H, ROW = 400, 240, 50
port = sys.argv[1]

CANDIDATES = [
    "/Games/Purchased/Dolphin_Splash.pdx",
    "/Games/Purchased/Cloudburst.pdx",
    "/Games/Purchased/GravityExpress.pdx",
    "/Games/Purchased/MiniMonsters.pdx",
    "/Games/Purchased/Root Bear.pdx",
    "/Games/Purchased/PlaydateFishing.pdx",
]
if len(sys.argv) > 2:
    CANDIDATES = sys.argv[2:]


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
            line.append(255 if (fb[y * ROW + (x >> 3)] >> (7 - (x & 7))) & 1 else 0)
        for _ in range(vscale):
            rows.append(b'\x00' + bytes(line))
    raw = b''.join(rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H * vscale, 8, 0, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def hold(fd, cmd, n=12, gap=0.05):
    for _ in range(n):
        os.write(fd, cmd); time.sleep(gap)


fd = open_port(port)
best = None
try:
    os.write(fd, b"echo off\r\n"); time.sleep(0.2)
    os.write(fd, b"autolock onBattery\r\n"); time.sleep(0.2)
    os.write(fd, b"dockcrank 0\r\n"); time.sleep(0.2)
    read_for(fd, 0.4)

    for game in CANDIDATES:
        name = game.rsplit('/', 1)[-1].replace('.pdx', '')
        print("\n--- %s ---" % name)
        os.write(fd, ("run %s\r\n" % game).encode())
        time.sleep(7.0); read_for(fd, 0.5)

        # a couple of taps to get past a title screen
        for _ in range(2):
            os.write(fd, b"btn a\r\n"); time.sleep(0.8)
        time.sleep(1.5)

        g0 = grab(fd)
        if not g0:
            print("   could not capture; skipping"); continue
        time.sleep(2.0)
        g1 = grab(fd)
        idle = pct(g0, g1)
        thresh = max((idle or 0) * 3.0, 1.5)
        print("   idle control: %.2f%%   (threshold %.2f%%)" % (idle, thresh))

        hold(fd, b"accel 1000 0 0\r\n"); time.sleep(0.7)
        ax = grab(fd)
        hold(fd, b"accel -1000 0 0\r\n"); time.sleep(0.7)
        ay = grab(fd)
        dx = pct(ax, ay)

        hold(fd, b"accel 0 1000 0\r\n"); time.sleep(0.7)
        bx = grab(fd)
        hold(fd, b"accel 0 -1000 0\r\n"); time.sleep(0.7)
        by = grab(fd)
        dy = pct(bx, by)

        got = max(d for d in (dx or 0, dy or 0))
        verdict = "RESPONDS TO TILT" if got > thresh else "no"
        print("   accel X +/-1g: %.2f%%   accel Y +/-1g: %.2f%%   -> %s"
              % (dx or -1, dy or -1, verdict))

        if got > thresh and (best is None or got > best[1]):
            best = (name, got)
            if ax: save_png(ax, 'tilt_%s_plus.png' % name)
            if ay: save_png(ay, 'tilt_%s_minus.png' % name)

        # back out to the launcher
        for _ in range(3):
            os.write(fd, b"btn b\r\n"); time.sleep(0.5)
        time.sleep(1.0)
finally:
    os.close(fd)

print("\n===== RESULT =====")
if best:
    print("accel DOES reach games: %s responded with %.2f%% change" % best)
else:
    print("no candidate responded to tilt — need a game known to use the accelerometer")
