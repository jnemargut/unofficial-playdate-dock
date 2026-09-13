# Implementation notes

The README is the front door. This is the part that only matters if you are
going to change something.

## Measured on real hardware

Numbers, not estimates. Everything here came off the device.

| | |
|---|---|
| Frame rate, silent | 33.7 fps |
| Frame rate, with sound | about 22 fps |
| Throughput | about 225 KB/s |
| Resync churn, before tightening the validator | 745,522 bytes |
| Resync churn, after | 4 bytes |
| IMU reading, board flat | 1.018 g on Z |

Motion injection was checked against unmodified games with an idle control run,
because "the game moved" is easy to imagine:

| Game | Movement with `accel` | Idle control |
|---|---|---|
| Dolphin Splash | 4.48% | 0.00% |
| MiniMonsters | 11.91% | 0.00% |

Input injection reaches games but **not** the System launcher.

## The stream protocol

Ported from Playdate Cabinet's mirrorpi. Everything below was observed on one
Playdate, or read out of that source. Where it disagrees with the
reverse-engineered descriptions floating around online, this is what my device
actually did:

- The message header is `uint8 opcode`, `uint8 arg`, `uint16 length`. The
  published description does not match what the device sends.
- Row numbers are **bit-reversed and 1-based**, both at once. Get either half
  wrong and you get a scrambled picture rather than an error, which is a
  miserable way to debug.
- Commands terminate with `\r\n`.
- `screen` dumps 12,000 bytes after a `~screen:\n` marker.
- Audio config is `stream a-` (off), `stream am` (mono), `stream a+` (stereo).
- `msg` caps at 251 characters.

**`stream enable` resets the device's audio configuration.** Anything set before
it is silently discarded. Playdate Cabinet's client sends the audio option from
`streamStarted()`, after enable, and so does this. Sending it during the
handshake produced a dock that believed it had sound and a device that had never
been told.

### Validator tightening

`PD_OP_APPLICATION` (0x99) originally accepted any size. A bogus header would
then swallow the following frame. Restricting it to the three sizes that
actually occur (0, 6, 48) took resync churn from 745,522 bytes to 4.

### Never write from a USB callback

This caused two separate hard-to-find failures. Both the connect handshake and
the accelerometer send were originally issued from inside the rx callback, and
both wedged the link under load. The handshake is now a main loop state machine
and the accel send happens at frame boundaries. The rx drain is bounded to 16
iterations so one busy tick cannot starve everything else.

## Video

### Two modes, chosen at boot

| Mode | Clock | Scale | Framebuffer |
|---|---|---|---|
| 800x480 | 295.2 MHz | exact 2x | 800x240, hardware doubles rows |
| 640x480 | 252 MHz | 1.6x, letterboxed | 640x480 |

The mode is a boot time choice because the clock and the DVI timing are set
before anything else starts, so the gesture saves the new mode in a watchdog
scratch register and reboots into it. Scratch registers survive a watchdog reset;
the SDK reserves 4 to 7 and leaves 0 to 3 free.

Scratch layout used here:

| Register | Holds |
|---|---|
| 0, 1 | hang counter, for the poll mode fallback |
| 2 | magic value, so stale scratch is not trusted |
| 3 | bit 0 video mode, bit 1 sound on, bit 2 announce the new size once |

### Half scanout

At 800x480 the picture is an exact 2x, so 240 of the 480 encoded lines are
duplicates. Setting the DVI engine's vertical repeat to 2 makes the hardware emit
each line twice and core 1 encodes half as many. Identical pixels, half the work:
core 1 goes from roughly 17% busy to 8.5%.

It is only on while a picture is streaming. The idle screen keeps all 480 rows,
because halving them halves the vertical resolution of its artwork and text to
buy headroom nothing is asking for.

**The switch is the delicate part.** Nothing in libdvi re-synchronises the
scanline queue to a vertical position, so the number of buffers core 1 pushes per
frame has to equal the number the IRQ consumes, every frame, forever. One buffer
of disagreement and the picture rolls and stays rolled. So core 1 latches the
change at its own frame boundary and publishes it to the IRQ in the same breath.
Core 1 always runs a few scanlines ahead, so the IRQ picks it up at the vertical
sync immediately before the frame in question, and both sides change together.
The IRQ runs on core 1 too, so there is no cross core ordering question.

### The scaler

Two lookup table paths, both folding bit reversal into the expansion so there is
only one pass over the 12,000 bytes in a frame:

- 800 mode: `fused2x[256]`, one source byte to two output bytes.
- 640 mode: `expand5to8[32]`, five source bits to one output byte.

The Playdate is MSB first and not inverted. Both were settled by measurement and
both stay switchable, so a wrong guess costs a keypress rather than a reflash.

### A bug worth remembering

The first version used `DVI_VERTICAL_REPEAT=2` with a 640x240 buffer, which
stretched 240 rows over 480 lines while the columns stayed at 400. The reasoning
was "240 maps 1:1", which is true of the row count and false of the aspect ratio.
It looked squashed on a TV. **Both axes must use the same factor.**

### Letterbox bands

The stream only paints the picture region, so in a letterbox mode the bands above
and below are never written again and whatever the idle screen left there stays
for the whole session. The transition into streaming clears the whole
framebuffer, which is cheaper than reasoning about which parts get repainted.

## Audio

Mono, 44.1 kHz, 16 bit signed little endian, into a 2048 sample stereo ring.
Mono goes to both channels.

Underrun emits digital silence, which is inaudible. Overflow drops the newest
samples. Both matter because the two ends run off different clocks that cannot be
adjusted and will drift over a session.

Payloads can be 2 KB against a 56 byte staging buffer, so audio streams straight
from the USB buffer in whatever chunks arrive. A straggler byte is carried
between chunks, since payloads are not guaranteed to split on sample boundaries.

Data islands are enabled once at boot and never disabled. Enabling them rebuilds
the scanline DMA lists, which must not happen underneath a running IRQ, and there
is nothing to gain: with no samples arriving the packets carry digital silence at
the same cost. The gesture only changes what the Playdate is asked to send.

### Clock regeneration

HDMI carries no audio clock. It sends N and CTS and the sink recovers the rate as
`128 * f_s = pixel_clock * N / CTS`. N is fixed at 6272 for 44.1 kHz, and
`128 * 44100 / 6272 = 900` exactly, so CTS is the pixel clock over 900:

| Mode | Pixel clock | CTS |
|---|---|---|
| 640x480 | 25.20 MHz | 28000 |
| 800x480 | 29.52 MHz | 32800 |

Both whole numbers. A fractional CTS is what makes cheap sinks warble, and
800x480 is in no lookup table, so this had to be computed.

### Data island room

A data island is 36 pixels, plus an 8 pixel preamble, plus 2 pixels of video
guard band. Both timings have room, the tightest fit being the 800x480 sync pulse
at 72 pixels.

## The badges

A rounded chip, white with a black border and black icon, top right, dissolving
over 1.6 seconds. A 1 bit screen has no alpha, so the fade is a dither that thins
out: solid, then half the pixels, then a quarter, then gone.

It cannot simply be written into the framebuffer once. Delta streaming only
repaints rows that changed, so an overlay left sitting there gets eaten away in
the moving parts of the picture and stays put in the still parts. It is redrawn
after every frame instead.

In 800 mode the framebuffer is 240 rows with the hardware doubling them, so the
badge halves its vertical coordinates to come out square.

White paper with black ink, not the other way round, because the Playdate's
screen is mostly white and a dark chip disappeared into it. The border is what
stops a white chip vanishing into a bright picture.

## Build gotchas

**`PICO_FLASH_SPI_CLKDIV` lives in boot2**, which is a separate shared CMake
target. Setting it on the executable does nothing at all. 800x480 booted to a
dead black screen with no LED until this was found. The fix:

```cmake
pico_define_boot_stage2(boot2_dock ${PICO_SDK_PATH}/src/rp2040/boot_stage2/compile_time_choice.S)
target_compile_definitions(boot2_dock PRIVATE PICO_FLASH_SPI_CLKDIV=4)
pico_set_boot_stage2(playdate_dock boot2_dock)
```

Verified at instruction level: the default writes `movs r1, #2`, ours writes
`movs r1, #4`.

**Bus priority.** Core 1 at 295 MHz is a heavy enough bus client to starve DMA,
which serves both the DVI scanout and every USB transfer. Both DMA read and write
are given priority.

**Homebrew's `arm-none-eabi-gcc` has no newlib** and fails with
`cannot read spec file 'nosys.specs'`. Use the official Arm GNU toolchain.

## Test harness warnings

The host tests misled me twice, both times by testing something other than what I
thought. Once by drawing a test pattern over the status screen, and once by
mixing `screen` dumps with an active binary stream so the marker could not be
found, which reported a failure while the stream was fine.

If a test fails, check the test.
