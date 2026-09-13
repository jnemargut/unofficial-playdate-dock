#!/usr/bin/env python3
"""Reboot an RP2040 running pico-sdk USB stdio into BOOTSEL.

pico_stdio_usb watches the CDC line coding and, when the host sets 1200 baud,
resets into the UF2 bootloader. Same trick the Arduino IDE uses. Needs no
picotool and no physical button.
"""
import os, sys, termios, time

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem11101'

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attrs = termios.tcgetattr(fd)
    # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    attrs[4] = termios.B1200
    attrs[5] = termios.B1200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    time.sleep(0.25)
finally:
    try:
        os.close(fd)
    except OSError:
        pass

print("touched %s at 1200 baud" % port)
