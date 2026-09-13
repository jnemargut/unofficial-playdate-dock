# The 14 pin header

Physical layout, as it sits on the board:

```
        1     2     3     4     5     6     7
  A    3V3   28    26    24*   22    17   GND
  B    29    27    25*   23    21*   16   GND

  * drives the RGB status LED
```

11 GPIO, one 3V3, two grounds. Three of the GPIO are spoken for by the LED,
leaving **8 clean pins: 16, 17, 22, 23, 26, 27, 28, 29**.

Not on this header at all: GPIO 8 to 15 and 18 to 20 are consumed internally by
the HDMI connector (TMDS pairs, clock, DDC and CEC), and GPIO 0 to 7 live on the
Flipper connector, where 2 to 5 are the IMU's SPI0 bus and 0 and 1 are Flipper's
own UART link.

## What is physically adjacent

Only two clean pairs share a column, and those are the easy ones to wire:

| Column | Pins | Why it matters |
|---|---|---|
| 2 | **28 / 27** | Both ADC. A two axis analog stick, side by side. |
| 6 | **17 / 16** | I2C0 or UART0. A bus on neighbouring pins. |

Every other clean pin is paired with an LED pin, so 22, 23, 26 and 29 are
singles as far as tidy wiring goes.

3V3 sits at one end of row A and both grounds at the far end, so power and
ground are at opposite corners.

## What each pin can be

**All four ADC inputs are on this header and all four are clean: 26, 27, 28, 29.**
That is the entire analog capability of the chip, available to you.

| Function | Clean options |
|---|---|
| ADC | 26 (ADC0), 27 (ADC1), 28 (ADC2), 29 (ADC3) |
| I2C0 | SDA 16 or 28, SCL 17 or 29 |
| I2C1 | SDA 22 or 26, SCL 23 or 27 |
| UART0 | TX 16 or 28, RX 17 or 29 |
| SPI1 | SCK 26, MOSI 27, MISO 28, CS 29 |
| Digital in/out | any of the 8 |

Two things worth noticing:

- **Both I2C buses are reachable independently.** I2C0 on 16/17 and I2C1 on
  22/23 leaves 26 to 29 entirely free.
- **A complete SPI1 bus lands on 26, 27, 28, 29.** SPI0 is not available; the
  IMU has it.

And a bigger one: **PIO1 is completely unused.** PicoDVI takes all three state
machines on PIO0, so PIO1's four are untouched. PIO can synthesise a UART, SPI,
or a bit banged protocol on *any* pin, so the table above is a list of the easy
routes, not the limit.

## Currently used by this firmware

| Pins | Used for | Free to reclaim? |
|---|---|---|
| 16, 17 | Debug UART at 115200 | Yes. Needs a 3V3 serial adapter to be useful at all, and everything diagnostic now shows on screen instead. |
| everything else | nothing | Yes |

So the honest answer is **6 pins free today, 8 if the debug UART goes**.

## Allocations worth considering

The one decision that matters early is whether to spend two pins on I2C, because
that is the difference between "a controller" and "as many peripherals as I ever
want" on one bus.

**A controller, with room to grow**
```
16, 17   I2C0        expander, Nunchuk, sensors, anything else later
22, 23   encoder     a real crank, mapped onto changecrank
26, 27   stick X/Y   analog, adjacent
28, 29   spare
```

**A controller, no I2C**
```
16, 17, 22   SNES / NES pad   clock, latch, data: d-pad, 4 buttons, start, select
23           extra button
26, 27       stick X/Y
28, 29       spare, or a second encoder
```

**Sensor rig**
```
16, 17   I2C0    GPS module, or anything on a bus
22, 23   I2C1    a second bus, so a slow device cannot block a fast one
26 - 29  ADC     four analog channels
```

## Why a controller is the natural first thing

The hard half is already proven on this hardware. `btn +a` and `changecrank`
reach **unmodified games**, and injecting them alongside a live stream costs
nothing measurable. A physical controller maps straight onto real Playdate
buttons and the crank, and no game ever needs to know it exists.

It also fixes the ergonomic problem the IMU has. The accelerometer is on the
module, so using tilt means waving around the thing with the HDMI lead, the
power feed and the Playdate all attached to it. A controller you hold while the
dock sits still is the right shape.

A rotary encoder on 22 and 23 is the piece I would want most. The crank is the
Playdate's whole personality, and right now it is stranded on a device sitting
in a cradle.
