# led100-c

A drop-in replacement for the lamp-driver board in an early solid-state pinball
machine.

The machine's MPU still speaks its original lamp-matrix protocol. This board
listens to that protocol and drives LEDs — which have replaced the machine's
incandescent lamps — through three chained TLC5947 constant-current drivers. The
whole point is the **fade**: real incandescent bulbs glow up and die away, and
LEDs switched hard on and off look wrong in a way you notice immediately. This
board puts the glow back.

It runs on a **Teensy 4.0** under the Arduino IDE with Teensyduino.

---

## How it works, in one picture

```
   MPU lamp matrix ──▶ decoder ──┐
                                 ├──▶ one 64-bit lamp bitmap ──▶ fade engine ──▶ TLC5947 ──▶ LEDs
   bench demo modes ─────────────┘
```

The MPU and the built-in demo modes are interchangeable sources: both hand off a
completed "which lamps are lit" bitmap the exact same way, so everything after
that point behaves identically whether you're driving from a real machine or
from a bench pattern.

---

## Hardware

- **Teensy 4.0** — the microcontroller.
- **Interface board** — sits between the MPU and the Teensy. The MPU's signals
  are 5 V and the Teensy's pins are 3.3 V-only, so the interface board handles
  the level shifting. **Heads up:** the current prototype interface board may
  have damaged some Teensy input pins by feeding them 5 V directly; a redesign is
  on the way. Until it lands, treat the live-decode path as unverified (the fade
  side is fully working — see [Status](#status)).
- **Three TLC5947 drivers**, chained — 24 channels each, 72 total, of which 60
  correspond to the original machine's lamps.
- **LEDs** in place of the original incandescent lamps.
- A **momentary push-button** (demo/mode select) and a **potentiometer** (fade
  speed) on the front panel.

### Pin map

The authoritative copy of this lives in the `#define`s at the top of
[`v0-1.ino/v0-1.ino.ino`](v0-1.ino/v0-1.ino.ino) — if these ever disagree,
believe the code.

| Signal | Teensy pin | Notes |
| --- | --- | --- |
| AD0–AD3 | 3, 2, 1, 0 | Address lines from the MPU |
| PD0–PD3 | 5, 6, 7, 8 | Group-enable lines (the original board's W/X/Y/Z) |
| STROBE | 4 | Address latch |
| TLC clock / data / latch / blank | 13 / 12 / 14 / 15 | To the first TLC5947 |
| Demo button | 11 | Momentary to ground |
| Fade-speed pot | A9 | Wiper to the pin, ends to 3.3 V and ground |

Signal names follow the original board's schematic, so the code, a scope probe,
and the machine's service manual all use the same labels.

### Wiring the TLC5947 BLANK pin

This is the one wiring detail most likely to trip you up, so it's called out
here as well as in the driver header. Best option first:

1. **BLANK → a Teensy pin, with a pull-up resistor to 3.3 V** (what this board
   does). Clean startup, no dimming. Recommended.
2. **BLANK tied to ground**, not wired to the Teensy. Works, but you'll get a
   brief flash of garbage at power-on and slightly dimmer LEDs *while they're
   fading*. Neither is fixable in software.

The full explanation is in the header comment of
[`v0-1.ino/TLC5947.h`](v0-1.ino/TLC5947.h).

---

## Using it

**Normal operation** needs no interaction: power it up and it decodes the MPU
and drives the lamps.

**The push-button** cycles through a live mode and three self-contained bench
patterns, in order:

| Press | Mode | What it does |
| --- | --- | --- |
| — | **Live** | Decode the real MPU input (the default at power-on) |
| 1 | **Random flicker** | ~20% of lamps lit, reshuffled a few times a second |
| 2 | **Blink** | All lamps on / all off, ~half a second each |
| 3 | **Chase** | Steps through all 60 lamps one at a time |
| 4 | back to **Live** | |

The demo modes ignore the MPU entirely — handy for testing the LEDs and the fade
feel on the bench with nothing else connected.

**The potentiometer** sets the fade speed. Fade-out is always slower than
fade-in, mimicking how a filament cools more slowly than it heats. It's meant to
be set once with the backbox open and then left alone.

---

## Configuring it

The knobs a builder actually touches, all near the top of
[`v0-1.ino/v0-1.ino.ino`](v0-1.ino/v0-1.ino.ino):

- **`ENABLE_DEMO_MODES`** — set to `0` for a final install to drop the demo
  patterns and the mode button, leaving only live decode.
- **`FADE_IN_CURVE` / `FADE_OUT_CURVE`** — the shape of the glow-up and
  die-away. Pick from the curves the fade engine offers
  ([`v0-1.ino/FadeEngine.h`](v0-1.ino/FadeEngine.h)).
- **`FADE_IN_MIN_MS` / `FADE_IN_MAX_MS` / `FADE_OUT_RATIO`** — the range the pot
  sweeps, and how much slower fade-out is than fade-in.
- **`NUM_TLC5947`** — number of chained driver chips, if your board differs.
- **Software vs. hardware SPI** — the driver supports both. This board currently
  uses bit-banged software SPI (the constructor call just below the pin
  `#define`s); the driver header explains the switch.

The **lamp map** — which of the 72 driver channels each incoming lamp lights — is
the `channelToLampBit` table in the sketch. It's specific to the machine and its
wiring, and is the thing to edit if a lamp lights the wrong LED.

---

## Building & flashing

1. Install the **Arduino IDE** and **[Teensyduino](https://www.pjrc.com/teensy/td_download.html)**.
2. Open [`v0-1.ino/v0-1.ino.ino`](v0-1.ino/v0-1.ino.ino).
3. Select **Tools → Board → Teensy 4.0**.
4. Upload.

No libraries to install: the `TLC5947` driver and the `FadeEngine` both live in
the sketch folder alongside the `.ino`.

---

## Status

- **Fade engine — working.** Demonstrated by the demo modes; this is the mature
  part of the project.
- **Live MPU decode — unverified.** The logic is written and reviewed, but can't
  be trusted on hardware until the redesigned interface board arrives (see
  [Hardware](#hardware)).
- **Lamp map — not yet reconciled.** `channelToLampBit` hasn't been checked
  against the real machine and is known to be at least partly wrong: a few driver
  channels are currently mapped to lamp positions that can never light, and a few
  real lamps aren't mapped to any channel. Flagged in the code; a proper pass is
  pending.
