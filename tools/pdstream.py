#!/usr/bin/env python3
"""Exercise the Playdate stream protocol from the Mac.

Mirrors the C parser in playdate-dock/src/pd_stream.c exactly, so anything that
works here should work on the module — and anything that breaks here would have
been undebuggable on a chip with no debugger.

Usage:
  pdstream.py <port> capture [seconds]     stream and save a real frame as PNG
  pdstream.py <port> acceltest [seconds]   stream WHILE injecting accel commands
"""
import os, sys, termios, time, select, zlib, struct

W, H, ROW = 400, 240, 50

OP_DEVICE_STATE = 1
OP_FRAME_BEGIN_DEPR = 10
OP_FRAME_END = 11
OP_FRAME_ROW = 12
OP_FRAME_BEGIN = 13
OP_FULL_FRAME = 14
OP_AUDIO_FRAME = 20
OP_AUDIO_CHANGE = 21
OP_AUDIO_OFFSET = 22
OP_APPLICATION = 0x99


def rev8(n):
    n = ((n & 0x55) << 1) | ((n & 0xAA) >> 1)
    n = ((n & 0x33) << 2) | ((n & 0xCC) >> 2)
    n = ((n & 0x0F) << 4) | ((n & 0xF0) >> 4)
    return n & 0xFF


def header_valid(opcode, arg, sz):
    if opcode == OP_DEVICE_STATE:     return sz == 8
    if opcode == OP_FRAME_BEGIN_DEPR: return sz == 0
    if opcode == OP_FRAME_BEGIN:      return sz == 4
    if opcode == OP_FRAME_END:        return sz == 0
    if opcode == OP_FRAME_ROW:        return sz == 52
    if opcode == OP_FULL_FRAME:       return sz == 34 + arg * 50
    if opcode == OP_AUDIO_FRAME:      return sz < 2048
    if opcode == OP_AUDIO_CHANGE:     return sz == 2
    if opcode == OP_AUDIO_OFFSET:     return sz == 4
    if opcode == OP_APPLICATION:      return True
    return False


class Parser:
    def __init__(self):
        self.fb = bytearray(ROW * H)
        self.buf = bytearray()
        self.frames = 0
        self.rows = 0
        self.bad = 0
        self.resyncs = 0
        self.opcodes = {}

    def feed(self, data):
        self.buf += data
        while True:
            if len(self.buf) < 4:
                return
            opcode, arg = self.buf[0], self.buf[1]
            sz = self.buf[2] | (self.buf[3] << 8)
            if not header_valid(opcode, arg, sz):
                # Slide one byte and retry — this is how we walk out of echoed
                # ASCII back into the binary stream.
                self.bad += 1
                del self.buf[0]
                continue
            if len(self.buf) < 4 + sz:
                return
            payload = bytes(self.buf[4:4 + sz])
            del self.buf[:4 + sz]
            self.opcodes[opcode] = self.opcodes.get(opcode, 0) + 1
            self.dispatch(opcode, arg, payload)

    def dispatch(self, opcode, arg, p):
        if opcode == OP_FRAME_ROW:
            row = rev8(p[0])
            if 1 <= row <= H:
                self.fb[(row - 1) * ROW:(row - 1) * ROW + ROW] = p[1:1 + ROW]
                self.rows += 1
        elif opcode == OP_FRAME_END:
            self.frames += 1
        elif opcode == OP_FULL_FRAME:
            mask = p[4:34]
            off = 34
            for r in range(H):
                if mask[r >> 3] & (1 << (r & 7)):
                    self.fb[r * ROW:r * ROW + ROW] = p[off:off + ROW]
                    off += ROW
                    self.rows += 1
            self.frames += 1


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(a[6]); cc[termios.VMIN] = 0; cc[termios.VTIME] = 0; a[6] = cc
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def write(fd, s):
    os.write(fd, s.encode())


def save_png(fb, path, invert=False, bitrev=True, vscale=2):
    rows = []
    for y in range(H):
        line = bytearray()
        for x in range(W):
            b = fb[y * ROW + (x >> 3)]
            bit = (b >> (7 - (x & 7))) & 1 if bitrev else (b >> (x & 7)) & 1
            if invert:
                bit ^= 1
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


def read_avail(fd, seconds):
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


def main():
    port = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else 'capture'
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

    fd = open_port(port)
    p = Parser()
    try:
        write(fd, "echo off\r\n"); time.sleep(0.2); read_avail(fd, 0.3)
        write(fd, "stream enable\r\n"); time.sleep(0.1)
        write(fd, "stream fullframe\r\n")

        t_end = time.time() + secs
        next_poke = time.time() + 0.3
        next_accel = time.time() + 0.5
        accel_sent = 0
        phase = 0.0

        while time.time() < t_end:
            data = read_avail(fd, 0.05)
            if data:
                p.feed(data)
            now = time.time()
            if now >= next_poke:
                write(fd, "stream poke\r\n")
                next_poke = now + 0.3
            if mode == 'acceltest' and now >= next_accel:
                # Sweep a tilt vector so a game would visibly react.
                import math
                x = math.sin(phase); y = math.cos(phase); z = 0.0
                phase += 0.4
                write(fd, "accel %d %d %d\r\n" % (int(x * 1000), int(y * 1000), int(z * 1000)))
                accel_sent += 1
                next_accel = now + 0.05   # 20 Hz, well above what a game needs

        write(fd, "stream disable\r\n")
        time.sleep(0.1)
        read_avail(fd, 0.3)
    finally:
        os.close(fd)

    print("mode           : %s" % mode)
    print("frames         : %d  (%.1f fps over %.1fs)" % (p.frames, p.frames / secs, secs))
    print("rows           : %d" % p.rows)
    print("bad header byes: %d" % p.bad)
    if mode == 'acceltest':
        print("accel sent     : %d" % accel_sent)
    print("opcodes seen   : %s" % ", ".join("%d x%d" % (k, v) for k, v in sorted(p.opcodes.items())))

    out = 'pd_capture_%s.png' % mode
    save_png(p.fb, out)
    print("wrote          : %s" % out)


if __name__ == '__main__':
    main()
