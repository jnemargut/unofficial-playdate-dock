#!/usr/bin/env python3
"""Test the `screen` command: a one-shot 400x240 framebuffer dump.

The stream protocol is delta-encoded, so a fresh connection paints nothing until
pixels happen to change. `stream fullframe` was supposed to force a repaint but
emits no opcode 14 on this OS. `screen` is the documented alternative:
"Dump framebuffer data (400x240 bits)" = 12,000 bytes on demand.
"""
import os, sys, termios, time, select, zlib, struct

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


def read_for(fd, seconds):
    out = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                c = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if c:
                out += c
    return bytes(out)


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
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


fd = open_port(port)
try:
    os.write(fd, b"echo off\r\n"); time.sleep(0.2); read_for(fd, 0.3)
    os.write(fd, b"screen\r\n")
    data = read_for(fd, 2.0)
finally:
    os.close(fd)

print("received %d bytes" % len(data))
print("first 40 bytes: %r" % data[:40])

# The payload is preceded by an ASCII marker; find the last newline before a
# 12,000 byte run.
idx = data.find(b'screen')
if idx >= 0:
    nl = data.find(b'\n', idx)
    payload = data[nl + 1:]
else:
    payload = data
print("payload after marker: %d bytes (want 12000)" % len(payload))

if len(payload) >= 12000:
    save_png(payload[:12000], 'pd_screen_dump.png')
    print("wrote pd_screen_dump.png")
else:
    print("SHORT — got %d" % len(payload))
