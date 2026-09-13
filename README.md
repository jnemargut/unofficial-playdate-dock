# Unofficial Playdate Dock

**Put your Playdate on the telly.**

Firmware that turns a Flipper Zero Video Game Module into an HDMI dock for the
Panic Playdate. Plug the Playdate into the module, plug the module into your TV,
and the little yellow screen shows up big. There's sound if you want it, and you
change settings by tilting the board.

It runs at about 34 frames a second, fills the whole screen, and works with any
game, because as far as the Playdate is concerned nothing unusual is happening.
You don't install anything on the Playdate itself.

> **This is an unofficial fan project.** It is not made by Panic, not endorsed by
> Panic, and not endorsed by Flipper Devices either. Playdate is Panic's. Flipper
> Zero is Flipper Devices'. I just wired them together.

---

## The five minute version

1. Buy a [Video Game Module for Flipper Zero](https://flipper.net/products/video-game-module-for-flipper-zero).
2. Grab `playdate_dock.uf2` from this repo.
3. Hold the button on the module while plugging it into your computer. A drive
   called `RPI-RP2` shows up. Copy the file onto it and the drive disappears,
   which means it worked.
4. HDMI cable from the module to your TV. Power to the module. Playdate into the
   module's USB port.
5. Unlock the Playdate and you should have a picture.

---

## What you need

**A Flipper Zero Video Game Module**, straight from Flipper:
**https://flipper.net/products/video-game-module-for-flipper-zero**

It's sold as a games accessory for the Flipper Zero, but the hardware inside is
an RP2040 with an HDMI socket, a USB port and a motion sensor. That covers every
part of this project.

**A Playdate**, awake and unlocked.

**Two USB-C cables and an HDMI cable.** One cable powers the module, one connects
the Playdate to it.

> **Use data cables.** This one cost me an evening. A charge-only USB-C cable
> will power the Playdate happily and the module will never see it, so you get a
> dock sitting there saying "no Playdate yet" next to a Playdate that is visibly
> charging.

### You don't need a Flipper Zero

The module clips onto a Flipper, and if you have one it works nicely as a power
source and a stand. It isn't required though, and the firmware never talks to it.

Power the module however you like:

| How | Notes |
|---|---|
| Clipped to a Flipper Zero | Tidy, and the Flipper makes a decent stand |
| USB-C from a phone charger | Simplest option |
| USB-C from a power bank | Makes the whole thing portable |
| USB-C from your computer | Handy while you're tinkering |

It wants 5V and draws a couple of hundred milliamps, plus whatever the Playdate
pulls while charging. Any normal charger will do.

---

## Flashing it

1. Unplug the module.
2. Hold down the little button on it.
3. Plug it into your computer while still holding the button.
4. Let go. A USB drive called `RPI-RP2` appears.
5. Copy `playdate_dock.uf2` onto it.
6. The drive disappears on its own once the copy finishes.

Or from a terminal:

```sh
cp playdate_dock.uf2 /Volumes/RPI-RP2/      # macOS
cp playdate_dock.uf2 /media/$USER/RPI-RP2/  # Linux
```

`picotool` works too if you prefer it.

To check it took, plug in HDMI and power with no Playdate attached. You should
get a little animated Playdate with a crank that turns, and text telling you to
plug one in.

---

## Using it

### Tilt gestures

The module has a motion sensor on board, so the controls are gestures rather than
buttons you'd have to find in the dark.

| Do this | Get this |
|---|---|
| Turn the board upside down and hold for 2 seconds | Switches between 800x480 and 640x480 |
| Tip the board onto its edge and hold for 2 seconds | Turns sound on and off |

They're on different axes so you can't trigger one while meaning the other. The
LED lights up after about 0.7 seconds to tell you the gesture has been noticed,
and the thing itself happens at 2 seconds, so putting the board back down before
then cancels it. Afterwards a little chip appears in the corner of the screen and
fades out, so you can see which way it went.

### The two picture modes

| Mode | What it looks like |
|---|---|
| 800x480 | Fills the whole screen, exactly 2x. Text and edges are crisp. |
| 640x480 | Letterboxed, 1.6x. Bars top and bottom. |

Both are worth trying, because the tradeoff is genuinely a matter of taste. The
Playdate has no grey pixels. It fakes grey by dithering black and white dots, so
doubling the picture also doubles the dither and your eye starts picking out
individual dots instead of blending them. Greys look chunky at 800x480 for that
reason. The 1.6x of 640x480 scrambles the dither grid rather than cleanly
doubling it, so greys often read as fine noise instead, which some people prefer.
Text goes the other way and looks better at 800x480.

### Sound

Sound is off by default because it costs you frame rate. The Playdate sends video
and audio down the same single USB connection, so every byte of sound is a byte
of picture that doesn't arrive:

| | Frame rate |
|---|---|
| Silent | about 34 fps |
| With sound | about 22 fps |

There's no firmware trick that gets around this, since it's one pipe. Sound is
mono at 44.1 kHz, which is already the cheaper of the two options the Playdate
offers. Stereo costs twice the bandwidth and would leave almost nothing for the
picture.

Tip the board on its edge to try it, and again to get your frames back.

### Motion control

The motion sensor also works the other way round. Instead of gesturing at the
dock, you tilt the dock and the Playdate thinks it is being tilted.

This works on unmodified games, with nothing written to support it. I tested it
on Dolphin Splash and MiniMonsters and both respond as if you were waving the
handheld around.

It's off by default and lives behind the debug console, because sending motion
upstream steals bandwidth from the video coming down, and it's expensive:
throughput drops from 160 KB/s to 20 KB/s with it running. It's in here as a
party trick, and because the groundwork is done if anyone wants to take it
further.

The version that would actually be good is a controller on the expansion header,
rather than tilting a board with three cables hanging off it.
[PINOUT.md](PINOUT.md) maps the 14 pin header and works out what fits where.
There are eight usable GPIO, all four analog inputs, both I2C buses, and an
entire unused PIO block.

---

## Running it on a plain Pico

You don't need the Video Game Module for this. It's ordinary RP2040 firmware and
runs on a Raspberry Pi Pico once you sort out two things.

### 1. Tell it where your HDMI pins are

One line in [CMakeLists.txt](CMakeLists.txt):

```cmake
DVI_DEFAULT_SERIAL_CONFIG=picodvi_dvi_cfg    # the Video Game Module's wiring
```

Change it to whichever board you have. PicoDVI ships configs for a pile of them
in `common_dvi_pin_configs.h`:

| Board | Config |
|---|---|
| Pimoroni Pico DV Demo Base | `pimoroni_demo_hdmi_cfg` |
| Adafruit Feather DVI | `adafruit_feather_dvi_cfg` |
| Pico DVI Sock | `pico_sock_cfg` |
| Olimex RP2040 Pico PC | `Olimex_RP2040_PICO_PC_cfg` |
| Waveshare RP2040 PiZero | `waveshare_rp2040_pizero_hdmi_cfg` |

Rolling your own is four differential pairs and a handful of resistors, and the
[PicoDVI](https://github.com/Wren6991/PicoDVI) repo covers how to do it.

### 2. Get the Playdate plugged into it

This is the fiddly part, because the module has a proper USB host port and a bare
Pico doesn't.

The RP2040's USB controller can act as a host, and this firmware uses the native
one, so the silicon is not the problem. What the Pico lacks is 5V on the USB
connector, since that socket expects to be fed rather than to feed. Two ways
round it:

- A micro USB OTG adapter on the Pico's port, then a USB-C cable to the Playdate,
  with 5V wired to the Playdate's VBUS from your own supply.
- Or solder a USB-A socket to `USB_DM` and `USB_DP` on the Pico and give it 5V.

The Playdate won't appear on the bus at all until it has power, so check that
first if nothing shows up.

### What you lose

Gestures, because a bare Pico has no motion sensor. The firmware notices and
skips them rather than breaking, but you won't be able to change resolution or
sound without rebuilding. The sensor is an ICM-42688-P on SPI0 (SCK 2, MOSI 3,
MISO 4, CS 5), so wire one up the same way and gestures come back.

Picture and sound work the same as on the module.

### Pico 2 / RP2350

I haven't tried it. PicoDVI supports RP2350 and the clock speeds here are well
within what it can do, so it's probably close. Open a PR if you get it going.

---

## Building from source

You need CMake and an `arm-none-eabi` toolchain with newlib. That last part
matters on macOS, where the Homebrew `arm-none-eabi-gcc` ships without it and
fails with `cannot read spec file 'nosys.specs'`. Use the official
[Arm GNU toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
instead.

```sh
git clone https://github.com/jnemargut/unofficial-playdate-dock.git
cd unofficial-playdate-dock
./setup.sh
cmake -S . -B build -DPICO_SDK_PATH=$PWD/lib/pico-sdk
cmake --build build -j8
```

`setup.sh` fetches the Pico SDK and PicoDVI at the exact commits this was tested
against, then applies the patches in [patches/](patches/). Don't skip the
patches. One of them is the difference between working and a chip that hard locks
a few seconds after the Playdate connects, and it looks exactly like a hardware
fault when it happens. [patches/README.md](patches/README.md) explains all four.

### Tests

Most of the fiddly logic is a parser, and parsers can be tested on a laptop
rather than on a chip with no debugger:

```sh
mkdir -p build_host
cc -std=c11 -O1 -I test/shim -I src \
   test/host_test.c src/pd_stream.c src/pd_frame.c src/pd_font.c src/pd_audio.c \
   -o build_host/host_test && ./build_host/host_test
```

That's 69 checks, covering awkward chunk boundaries, audio riding alongside
video, the scaler geometry and the HDMI clock numbers. It also writes
`build_host/*.pgm` files so you can look at the screens without a TV.

---

## How it actually works

The Playdate can mirror its screen over USB. It isn't a USB display; it's a
serial console with a `stream` command that makes the device push its framebuffer
out as delta updates, each one a header plus a row number plus fifty bytes of
pixels. The client here is ported from
[Playdate Cabinet](https://github.com/daveatpanic/PlaydateCabinet), which is a
Playdate arcade cabinet project that talks the same protocol.

The module acts as the USB host, which is what makes the whole thing possible
with no computer in the setup. The RP2040 enumerates the Playdate itself, opens
the serial interface and drives the conversation.

HDMI comes out of a chip that has no video hardware at all. PicoDVI bit-bangs DVI
straight from PIO and DMA, at 252 MHz for 640x480 and 295.2 MHz for 800x480. Core
1 does nothing but feed it scanlines while core 0 handles USB. The sound rides in
the blanking time between scanlines, which plain DVI leaves empty and HDMI uses
for small data packets.

Three things surprised me while building it:

- The row numbers in the protocol are bit-reversed and 1-based, both at once.
  The reverse-engineered descriptions I found online also lay the message header
  out differently from what my device actually sent. Everything here was worked
  out by reading Playdate Cabinet's source and poking a real Playdate, so treat
  it as what one device does rather than as a specification.
- At 800x480 the picture is an exact 2x, so the firmware stores 240 rows and has
  the DVI engine emit each one twice. Same picture, half the work, and that
  saving is roughly what pays for the HDMI audio encoding.
- Flipping that setting while running is delicate, because nothing
  re-synchronises the scanline queue to a vertical position. Producer and
  consumer have to agree on the row count every single frame, or the picture
  rolls and stays rolled.

---

## What it doesn't do

It can't wake a locked Playdate. USB wakeup runs device to host, and here the
handheld is the host, so you have to press Lock yourself.

Data disk mode is no use. It's a different USB product ID with no serial
interface, so eject it and reboot the Playdate normally.

Mirroring isn't free for the Playdate, which has to push its framebuffer out
over USB on top of running the game. I haven't measured what that costs, so I
don't know how much it affects demanding games.

---

## Thanks

- [Luke Wren](https://github.com/Wren6991/PicoDVI) for PicoDVI. Bit-banging DVI
  out of a PIO block has no business working as well as it does.
- [Ian Jordan](https://github.com/ikjordan/PicoDVI) for the HDMI audio support on
  top of it.
- [daveatpanic](https://github.com/daveatpanic/PlaydateCabinet) for Playdate
  Cabinet, without which the stream protocol would still be a mystery to me.
- Flipper Devices, for building the board this runs on and documenting its GPIO
  properly enough that I could find my way around it.

MIT licensed. See [LICENSE](LICENSE).

[NOTES.md](NOTES.md) has the implementation detail if you want it: measured
numbers, protocol gotchas, why the scaler works the way it does, and the mistakes
that were expensive to find.
