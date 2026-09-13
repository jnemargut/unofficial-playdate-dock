#!/usr/bin/env python3
"""Minimal CDC-ACM serial helper using raw termios — no pyserial needed.

Usage:
  ser.py <port> <cmd>            send one line, print whatever comes back for ~1.5s
  ser.py <port> --raw <cmd> <s>  send one line, dump raw bytes for <s> seconds to stdout (binary-safe hex preview)
  ser.py <port> --listen <s>     just listen for <s> seconds
"""
import os, sys, termios, time, select

def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    # raw mode
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

def read_for(fd, seconds):
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if chunk:
                buf += chunk
    return bytes(buf)

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    port = sys.argv[1]
    fd = open_port(port)
    try:
        if sys.argv[2] == '--listen':
            secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
            data = read_for(fd, secs)
        elif sys.argv[2] == '--raw':
            cmd = sys.argv[3]
            secs = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
            os.write(fd, (cmd + '\r\n').encode())
            data = read_for(fd, secs)
        else:
            cmd = ' '.join(sys.argv[2:])
            os.write(fd, (cmd + '\r\n').encode())
            data = read_for(fd, 1.5)
        sys.stdout.write(data.decode('utf-8', 'replace'))
        sys.stdout.flush()
        sys.stderr.write("\n[%d bytes]\n" % len(data))
    finally:
        os.close(fd)
    return 0

if __name__ == '__main__':
    sys.exit(main())
