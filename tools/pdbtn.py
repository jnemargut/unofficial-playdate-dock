#!/usr/bin/env python3
"""Does `btn` actually reach the OS? With a proper control.

The launcher animates by itself, so "pixels changed" alone proves nothing. This
measures an idle baseline first, then compares injection against it.
Also tries both argument styles, since `help` documents names (+a/-a/a) while
Panic's client sends digits.
"""
import os, sys, termios, time, select

W, H, ROW = 400, 240, 50
port = sys.argv[1]


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
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, b"screen\r\n")
    d = read_for(fd, 1.2)
    m = d.find(b'~screen:\n')
    if m < 0: return None
    p = d[m + 9:]
    return p[:12000] if len(p) >= 12000 else None


def pct(a, b):
    if not a or not b: return None
    return 100.0 * sum(bin(x ^ y).count('1') for x, y in zip(a, b)) / (12000 * 8)


fd = open_port(port)
try:
    os.write(fd, b"echo off\r\n"); time.sleep(0.2); read_for(fd, 0.3)

    print("== control: how much does the launcher change on its own? ==")
    a = grab(fd); time.sleep(1.5); b = grab(fd)
    base = pct(a, b)
    print("   idle drift over 1.5s: %.2f%%" % base)

    for style, down, up in (("names", b"btn +right\r\n", b"btn -right\r\n"),
                            ("tap-name", b"btn right\r\n", None),
                            ("digits", b"btn +1\r\n", b"btn -1\r\n")):
        c = grab(fd)
        for _ in range(3):
            os.write(fd, down); time.sleep(0.12)
            if up: os.write(fd, up)
            time.sleep(0.5)
        time.sleep(0.6)
        d = grab(fd)
        got = pct(c, d)
        verdict = "MOVED" if (got is not None and got > max(base * 3, 1.0)) else "no"
        print("   btn style %-9s -> %.2f%% changed   %s" % (style, got if got is not None else -1, verdict))

    print("\n== does 'a' open something? (big change expected) ==")
    c = grab(fd)
    os.write(fd, b"btn a\r\n"); time.sleep(2.0)
    d = grab(fd)
    got = pct(c, d)
    print("   btn a -> %.2f%% changed  %s" % (got if got is not None else -1,
                                              "LAUNCHED SOMETHING" if got and got > 10 else "small change"))
    # back out
    os.write(fd, b"btn b\r\n"); time.sleep(1.5)
finally:
    os.close(fd)
