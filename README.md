**A drop-in replacement for early Stern & Bally lamp driver boards to make your lamps s~m~o~o~t~h again**

> ⚠️ **This is Rev 5 - work in progress.** 
> This board is still being developed and tested. Everything may change before release! 

---

## What is this?

Early Stern and Bally solid-state pinball machines (mid-1970s to the mid-80s) use incandescent bulbs. It's awesome to swap those bulbs for LEDs (less power draw, less heat, less maintenance)...  but: LEDs switch on and off instantly, and this makes old tables flash & strobe in a nasty way.

The LED-100 fixes that. You install it in place of the original board. It reads the lamp codes exactly like the original did, and drives your LEDs with a gentle fade - keep that warm glowing feel without the actual heat - your LEDs will behave more like real bulbs.

**Will this work for your machine?** See [Compatibility](#compatability) below.

---

## Will it work on my machine?

Two things have to be true:
1. Your machine was designed to use one of the supported lamp driver boards.
2. Every lamp this board controls has already been swapped with an LED. I don't know what it'll do if you try and drive incandescents.

**What this board does _not_ do:** it only drives the lamps that ran through the original lamp driver board. It has nothing to do with your machine's GI or any high-current feature/flasher lamps that are driven from the solenoid driver board. Those are separate systems and are not controlled by this board.

---

## How to use it

Once installed, there's nothing to operate - power the machine on and tit does what it does.

There are a couple of controls on the board.

- **s~m~o~o~t~h knob** - Turn this to adjust the fade-speed. All the way up will probably be way too gooey, all the way off will essentially diable the board, and your LEDs will snap on and off like before. (Fade-out is always a little slower than fade-in, 'cause that's now incandescent lamps work.)
- **A button and another knob** - These are placeholders right now. The button currently cycles through some demo/test modes, which ignore input from the MPU. The knob tweaks overall brightness.

---

## Installing it

The board is fully pin-compatible with the original — same connectors, same `J1`–`J4` labels, so swapping it in is straightforward:

1. **Turn off and unplug the machine.** C'mon.
2. Unplug the cables from the original lamp driver board. You might want to label the cables according to the markings on the board - `J1`, `J2`, `J3`, and `J4`
3. Unbolt and remove the original board from the backbox†.
4. Bolt the LED-100 into the backbox*.
5. Plug the same four cables onto the matching headers on the LED-100, `J1`-`J4`.  These are keyed, so check that they are oriented correctly.

†**Mounting (Rev 5)**: This board revision is much smaller than the original, so the mounting holes don't line up with the factory bolt points. You can get one bolt in, but not the rest. 
The final version will have a better mounting solution, but for now: secure it with the single bolt that does line up, and make sure no pins, traces, or solder joints on the back of the board are touching metal.

---

## What's in this git repository

Everything you need to understand, build, modify, or reproduce the board:

- **[`LED-100-hardware/`]()** — the circuit board: KiCad schematic and layout, plus the gerbers used to produce this rev.
- **[`LED-100-firmware/`]()** — the code that runs on the board's Teensy 4.0, with its own [README]() covering how it works, the pin map, and how to build and flash it.

---

## How It Works, the short technical version

The board reads the machine's original lamp-matrix signals, works out which lamps should be lit, and drives LEDs through three PWM LED drivers (TLC5947). A [Teensy 4.0] is the brain that runs the whole thing. The fade curves are calculated is done in software, so the glow-up and fade-out curves are adjustable. Full detail is in the [firmware README]().

Here's how the PCB looks in KiCad:
<img width="610" src="https://raw.githubusercontent.com/andrewduhan/LED-100/refs/heads/main/assets/pcb.png"> 

...and here's the schematic:
<img width="610" src="https://raw.githubusercontent.com/andrewduhan/LED-100/refs/heads/main/assets/schematic.png">

---

## Compatibility

> **NOTE:** The LED-100 doesn't yet work in any machine; this list is the _intended_ target, not a tested one. The **Stern LDA-100** is the active development board. Once it's working there, I'll try it in any compatible machine I can :)

The LED-100 is meant to replace several factory lamp driver boards, which are all electrically equivalent. 

| Board                 | Manufacturer |
| --------------------- | ------------ |
| Stern LDA-100         | Stern        |
| Stern LDB-100         | Stern        |
| Bally AS-2518-14      | Bally        |
| Bally AS-2518-23      | Bally        |
| Bally A084-91612-A000 | Bally        |

Not sure which board your machine has? Check the part number silkscreened on the original board, or your machine's service manual.

---

## License

Firmware and hardware are licensed under the terms in the LICENSE file, in their respective directories.
