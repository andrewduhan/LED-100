/*
  Minimal TLC5947 driver - buffer, setPWM, write. Adds two things the
  Adafruit library doesn't have:
    - Optional BLANK pin support, pulsed tightly around just the latch
      pulse (not the whole shift-out) to avoid mid-PWM-cycle glitches
      when new data lands - see write() for the datasheet reasoning.
    - A real hardware-SPI path (bulk SPI.transfer of packed bytes),
      alongside the original bit-banged software-SPI mode.
  No RGB convenience helpers - just the primitives.

  WIRING THE BLANK PIN
  --------------------
  BLANK high forces every output off; BLANK low lets the grayscale PWM
  run. Every latch also momentarily forces the outputs off (until the
  chip's next ~1ms PWM period), and the only way to hide that is to pulse
  BLANK - which resets the PWM counter so a fresh period starts at once.
  That gives three wiring choices, best first:

    1. BLANK -> a micro GPIO, with a pull-up resistor to VCC.
       Pass that pin as blankPin. The pull-up holds the outputs off
       through power-up until the micro takes over and drives BLANK low;
       thereafter this driver pulses BLANK around each latch. This is the
       only wiring with NEITHER a power-on garbage flash NOR any dimming
       during rapid updates.

    2. BLANK tied to GND on the board, not wired to the micro.
       Pass blankPin = -1. Outputs are always PWM-controlled. Expect two
       things, neither fixable in software: a brief garbage flash at
       power-on (the grayscale latch powers up undefined, and nothing can
       hold the outputs off until the first write), and ~10% dimming
       during rapid updates such as fades (the driver can't pulse BLANK to
       hide the per-latch blanking).

    3. BLANK -> a micro GPIO but tied/pulled some other way.
       Works, but only choice 1's pull-up gives the clean power-on.

  begin() shifts an all-zero frame and latches it, so the grayscale latch
  holds a defined off state as soon as begin() runs - minimizing, though
  in choice 2 not eliminating, the power-on flash.
*/
#ifndef TLC5947_H
#define TLC5947_H

#include <Arduino.h>
#include <SPI.h>

class TLC5947 {
public:
  // Software (bit-banged) SPI - any digital pins for clock/data.
  // blankPin is optional; pass -1 (default) if BLANK is tied low in
  // hardware rather than wired to a GPIO (see WIRING THE BLANK PIN above).
  TLC5947(uint16_t numDrivers, uint8_t clockPin, uint8_t dataPin,
          uint8_t latchPin, int8_t blankPin = -1);

  // Hardware SPI - uses the board's default SPI MOSI/SCK pins.
  TLC5947(uint16_t numDrivers, uint8_t latchPin, int8_t blankPin = -1);

  ~TLC5947();

  void begin();
  void setPWM(uint16_t chan, uint16_t pwm);
  uint16_t getPWM(uint16_t chan) const;
  void write();

  // Drive the BLANK pin directly. write() already brackets its latch pulse
  // with these; call them yourself only to force the outputs dark and hold
  // them there. No-op if no blank pin was configured.
  void outputsOn();    // BLANK low  - outputs active
  void outputsOff();   // BLANK high - outputs forced dark

  // Hardware-SPI shift clock. The datasheet allows 30MHz for a lone TLC5947
  // but only 15MHz once they are cascaded, which is what we do - so 15MHz is
  // the real ceiling here, not 30. The 10MHz default leaves headroom for the
  // ribbon between the boards; a full 72-channel shift takes ~86us at that
  // rate, which is already nothing.
  void setSPIClockHz(uint32_t hz) { spiClockHz = hz; }

private:
  void allocPwmBuffer();   // shared by both constructors
  void shiftOutSoft();
  void shiftOutHardware();

  uint16_t numDrivers;
  uint16_t numChannels;
  uint16_t *pwmBuffer;

  bool useHardwareSPI;
  uint8_t clockPin, dataPin, latchPin;
  int8_t blankPin;
  uint32_t spiClockHz = 10000000UL;

  uint8_t *spiBuf;       // pre-allocated packed-byte buffer, hw SPI only
  uint16_t spiBufLen;
};

#endif
