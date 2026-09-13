#!/usr/bin/env python3
"""One command to see what's connected and what to do next.

    python3 tools/checkup.py
"""
import subprocess, glob, os, sys

PD_VID = 4913          # 0x1331 Panic
PD_PID_SERIAL = 22336  # 0x5740 normal mode, serial console
PD_PID_DISK = 22337    # 0x5741 data disk, NO serial
RP_VID = 11914         # 0x2E8A Raspberry Pi


def ioreg():
    try:
        return subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w", "0"],
                              capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return ""


reg = ioreg()
ports = sorted(glob.glob("/dev/cu.usbmodem*"))
pd_port = next((p for p in ports if "PDU" in p or "PD" in os.path.basename(p)[9:11]), None)

def has_pid(pid):
    # ioreg prints this two ways: '"idProduct" = 22336' at the node level and
    # '"idProduct"=22336' inside the USB Device Info dict. Match either.
    return ('"idProduct" = %d' % pid) in reg or ('"idProduct"=%d' % pid) in reg


playdate_serial = has_pid(PD_PID_SERIAL) and "Playdate" in reg
playdate_disk = has_pid(PD_PID_DISK)
playdate_any = "Playdate" in reg
rp2_boot = "RP2 Boot" in reg
pico_cdc = '"USB Product Name" = "Pico"' in reg
volumes = os.listdir("/Volumes") if os.path.isdir("/Volumes") else []

print("=" * 62)
print(" Playdate Dock — checkup")
print("=" * 62)

print("\nMODULE (Flipper Video Game Module)")
if rp2_boot:
    print("  * in BOOTSEL — ready to flash")
    print("    cp playdate_dock.uf2 /Volumes/RPI-RP2/")
elif pico_cdc:
    print("  * running STOCK Flipper firmware (enumerates as 'Pico')")
    print("    to flash: python3 tools/bootsel.py %s" %
          (ports[0] if ports else "/dev/cu.usbmodemXXXX"))
else:
    print("  * not visible on USB")
    print("    This is CORRECT if it is running our firmware — host mode means")
    print("    it no longer enumerates as a device. Check the LED instead:")
    print("      blue = waiting for a Playdate   green = streaming")
    print("      amber = Playdate in data-disk mode   red = unknown device")
    print("    To reflash: hold BOOT while plugging in USB, or use the Flipper's")
    print("    Video Game Module Tool over SWD.")

print("\nPLAYDATE")
if playdate_serial and pd_port:
    print("  * connected, normal mode, console ready: %s" % pd_port)
elif playdate_disk or "PLAYDATE" in volumes:
    print("  * connected but in DATA DISK mode — no serial console there")
    print("    diskutil eject /Volumes/PLAYDATE")
elif playdate_any:
    print("  * present on USB but no serial port yet; give it a moment")
else:
    print("  * not connected")
    print("    - press Lock to wake it (a locked Playdate drops off USB entirely)")
    print("    - use a DATA-capable USB-C cable; a charge-only one shows nothing")

print("\nOTHER SERIAL PORTS")
for p in ports:
    print("  %s%s" % (p, "   <-- Playdate" if p == pd_port else ""))
if not ports:
    print("  none")

print("\nNEXT")
if playdate_serial and pd_port:
    print("  Playdate is reachable. Useful one-liners:")
    print("    python3 tools/pdstream.py %s capture 5      # grab a real frame as PNG" % pd_port)
    print("    python3 tools/pdgame.py   %s <game.pdx>      # test injection in a game" % pd_port)
elif rp2_boot:
    print("  Flash the module, then reconnect the Playdate.")
else:
    print("  Nothing to do until a device is connected.")
print()
