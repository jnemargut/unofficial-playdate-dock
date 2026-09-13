# The patches

Four small changes to two upstream libraries. Every one of them is here because
the thing broke without it, and three of the four were found the hard way.

## `tinyusb.patch`

Applies to `lib/pico-sdk/lib/tinyusb`, file
`src/portable/raspberrypi/rp2040/hcd_rp2040.c`.

**`panic("Data Seq Error")` becomes a counter.** This is the important one. The
RP2040's USB hardware shares one set of handshake latches between endpoints, so
a transfer that was actually acknowledged can show up as a sequence error when
the bus is busy. Upstream's response is `panic()`, which on RP2040 is an
infinite loop: the chip is simply dead until something resets it. At the data
rate a video stream runs at, that happens within seconds. Counting the error and
carrying on is correct, because the error is not real.

**`busy_wait_at_least_cycles(12)` becomes `96`.** The delay this stands in for
is measured in 48 MHz USB cycles, about 250 ns, but the function counts CPU
cycles. At the stock 125 MHz those happen to be close enough. This firmware runs
at 295.2 MHz, where 12 cycles is 41 ns, roughly six times too short, and
transfers start failing intermittently. See tinyusb issue #3533.

**`panic("hcd_clear_stall")` becomes `return true`.** Same reasoning, smaller
stakes.

## `picodvi-audio.patch`

Applies to `lib/PicoDVI-audio`.

**Vertical repeat becomes a runtime value.** It was a compile-time macro, and
this firmware carries two output modes in one binary and switches between full
and half scanout while running. It is now a field on `struct dvi_inst`, written
through `vertical_repeat_mask_next` and latched during vertical sync so the
change always lands on a frame boundary. That last part matters more than it
sounds: nothing re-synchronises the scanline queue to a vertical position, so
one buffer of disagreement between producer and consumer rolls the picture and
it stays rolled.

**The AVI infoframe stops claiming a video code it does not have.** It was
hardcoded to tell the TV that any timing which is not 720 wide is 640x480p60.
This firmware also runs 800x480, which appears in no CEA table, and telling a TV
an 800 pixel wide picture is 640x480 invites it to rescale something that was
arriving perfectly well. It now sends "no standard format" instead.
