#include "TLC5947.h"

// The datasheet's minimum XLAT/BLANK high pulse (T_WH1) is 30ns, and its
// XLAT-rise-to-BLANK-fall setup (T_SU2) is another 30ns. Back-to-back
// digitalWrite() calls on a 600MHz Teensy 4 probably clear both on their own,
// but "probably" is doing a lot of work in that sentence and the margin is
// nil. 50ns costs us nothing at a 5ms write cadence, so buy the margin.
static inline void pulseGuard() {
#if defined(__IMXRT1062__)   // Teensy 4.x
  delayNanoseconds(50);
#else
  delayMicroseconds(1);
#endif
}

// Sets numChannels from numDrivers and allocates the zeroed PWM buffer.
// Both constructors need exactly this, so it lives here rather than being
// copy-pasted into each.
void TLC5947::allocPwmBuffer() {
  numChannels = 24 * numDrivers;
  pwmBuffer = new uint16_t[numChannels]();   // () value-inits every channel to 0
}

TLC5947::TLC5947(uint16_t n, uint8_t clk, uint8_t data, uint8_t latch, int8_t blank)
  : numDrivers(n), useHardwareSPI(false), clockPin(clk), dataPin(data),
    latchPin(latch), blankPin(blank), spiBuf(nullptr), spiBufLen(0) {
  allocPwmBuffer();
}

TLC5947::TLC5947(uint16_t n, uint8_t latch, int8_t blank)
  : numDrivers(n), useHardwareSPI(true), clockPin(255), dataPin(255),
    latchPin(latch), blankPin(blank) {
  allocPwmBuffer();

  spiBufLen = (numChannels / 2) * 3;   // 2 channels (24 bits) pack into 3 bytes
  spiBuf = new uint8_t[spiBufLen];
}

TLC5947::~TLC5947() {
  delete[] pwmBuffer;
  delete[] spiBuf;   // delete[] on nullptr (software-SPI case) is a no-op
}

void TLC5947::begin() {
  pinMode(latchPin, OUTPUT);
  digitalWrite(latchPin, LOW);

  if (blankPin >= 0) {
    // Order matters. pinMode(OUTPUT) starts driving the pin at whatever the
    // output register already holds, which is LOW - so setting the level
    // first is what stops BLANK from dipping low on the way up. The datasheet
    // is emphatic here: the grayscale latch powers up holding garbage, and
    // BLANK must be high at power-on or the outputs will briefly show it.
    //
    // This only covers the window from begin() onward. Between the LEDs
    // getting power and begin() running, BLANK is still an undriven input;
    // only a pullup resistor on the board can hold it high across that gap.
    digitalWrite(blankPin, HIGH);
    pinMode(blankPin, OUTPUT);
  }

  if (useHardwareSPI) {
    SPI.begin();
  } else {
    pinMode(clockPin, OUTPUT);
    pinMode(dataPin, OUTPUT);
    digitalWrite(clockPin, LOW);
  }

  // Give the grayscale latch a defined value (all channels off) as early as
  // we can, so it isn't showing its power-on garbage. The constructor already
  // zeroed pwmBuffer, so this just shifts it out and latches.
  //
  // A bare latch pulse, deliberately NOT the BLANK-bracketed write(): there is
  // no live picture to protect from a mid-PWM glitch, and write()'s trailing
  // outputsOn() would drive BLANK low - which would defeat the whole point in
  // the with-BLANK case by un-holding the outputs. Leaving BLANK as it stands
  // (high where we have the pin, so still hard-off; hardware-tied otherwise)
  // is what keeps this safe in every wiring.
  if (useHardwareSPI) shiftOutHardware();
  else shiftOutSoft();
  digitalWrite(latchPin, HIGH);
  pulseGuard();
  digitalWrite(latchPin, LOW);
}

void TLC5947::setPWM(uint16_t chan, uint16_t pwm) {
  if (chan >= numChannels) return;
  if (pwm > 4095) pwm = 4095;
  pwmBuffer[chan] = pwm;
}

uint16_t TLC5947::getPWM(uint16_t chan) const {
  if (chan >= numChannels) return 0;
  return pwmBuffer[chan];
}

void TLC5947::shiftOutSoft() {
  for (int32_t c = (int32_t)numChannels - 1; c >= 0; c--) {
    for (int8_t b = 11; b >= 0; b--) {
      digitalWrite(clockPin, LOW);
      digitalWrite(dataPin, (pwmBuffer[c] & (1 << b)) ? HIGH : LOW);
      digitalWrite(clockPin, HIGH);
    }
  }
  digitalWrite(clockPin, LOW);
}

void TLC5947::shiftOutHardware() {
  // Pack pairs of 12-bit channels into 3 bytes (24 bits), MSB first, highest
  // channel index first. That matches both the bit-banged path above and the
  // chip's own layout: the 288-bit register runs from OUT0's LSB at bit 0 up
  // to OUT23's MSB at bit 287, and data shifts toward the MSB end - so the
  // first bit clocked in has to be the highest channel's MSB.
  //
  // Note SPI.transfer(buf, len) is full-duplex and writes the received bytes
  // back over spiBuf. Harmless, since we repopulate it from scratch every
  // call, but don't expect its contents to survive.
  uint16_t bi = 0;
  for (int32_t c = (int32_t)numChannels - 1; c >= 1; c -= 2) {
    uint16_t hi = pwmBuffer[c];
    uint16_t lo = pwmBuffer[c - 1];
    spiBuf[bi++] = hi >> 4;
    spiBuf[bi++] = ((hi & 0xF) << 4) | (lo >> 8);
    spiBuf[bi++] = lo & 0xFF;
  }
  SPI.beginTransaction(SPISettings(spiClockHz, MSBFIRST, SPI_MODE0));
  SPI.transfer(spiBuf, spiBufLen);
  SPI.endTransaction();
}

void TLC5947::write() {
  if (useHardwareSPI) shiftOutHardware();
  else shiftOutSoft();

  // Blank across the latch pulse, and only the latch pulse.
  //
  // An XLAT rising edge on its own forces every output off until the next
  // grayscale display period begins, and it does NOT reset the grayscale
  // counter. So a bare latch would leave the LEDs dark for whatever remains
  // of the period in flight - averaging half of one, and a period is 4096
  // steps of a 4MHz internal oscillator, so ~0.5ms. At our write rate that
  // is a real, and worse, a fade-only dimming.
  //
  // Pulsing BLANK sidesteps that: BLANK high resets the grayscale counter to
  // zero, so dropping it low again starts a fresh display period immediately
  // rather than waiting out the old one. Hence blank, latch, unblank.
  //
  // Blanking across the whole shift-out instead would be far worse - that's
  // hundreds of microseconds dark rather than hundreds of nanoseconds.
  outputsOff();
  digitalWrite(latchPin, HIGH);
  pulseGuard();                    // XLAT high >= 30ns (T_WH1)
  digitalWrite(latchPin, LOW);
  pulseGuard();                    // XLAT high -> BLANK low >= 30ns (T_SU2)
  outputsOn();
}

void TLC5947::outputsOn() {
  if (blankPin >= 0) digitalWrite(blankPin, LOW);
}

void TLC5947::outputsOff() {
  if (blankPin >= 0) digitalWrite(blankPin, HIGH);
}
