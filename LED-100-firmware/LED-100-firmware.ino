/*
  Lamp-matrix decoder -> fade engine -> TLC5947 driver
  Teensy 4.0 / TeensyDuino

  Replaces the lamp-driver board in an early solid-state pinball machine.
  The MPU still speaks its original lamp-matrix protocol; this board decodes it, and drives LEDs (which have replaced the machine's
  incandescent lamps) through three chained TLC5947 constant-current drivers. The point of the exercise is the fade: incandescent 
  lamps glow up and die away, and LEDs switched hard on and off look wrong in a way you notice immediately.

  Signal names match the original board's schematic, so that this code, a scope probe and the service manual all agree:
    AD0..AD3  - the 4 address lines
    PD0..PD3  - the 4 group-enable lines (the original W/X/Y/Z)
    STROBE    - address latch

  PROTOCOL
  --------
  - All 9 input lines idle HIGH -> INPUT_PULLUP throughout.
  - AD0..AD3 carry an address 0-15, latched on STROBE's rising edge (STROBE is active-HIGH).
  - PD0..PD3 are active-LOW, driven by 4 parallel latching decoders (MC14514B) fed from the same address lines. Critically, they 
    assert *after* STROBE - each pulses low independently, sometime before the next STROBE. They are NOT sampled at the same 
    instant as the address. A falling edge on one of them means "the address STROBE most recently latched applies to my group", 
    so capture reacts to PD edges directly rather than reading all four at strobe time.
  - Addresses 0-14 are the 15 data addresses. 15 addresses x 4 groups = the 60 lamps of the original matrix. A lamp is identified 
    throughout this file by its bit index:  lampBit = pd * 16 + addr.
  - Address 15 is the end-of-frame sentinel and carries no data.
  - Frames arrive tied to AC zero-crossing (~120/sec), not a fixed clock. A whole 16-strobe frame completes well inside one 
    ~8.3ms half-cycle, so there is no time pressure on any ISR here.

  SHARING THE BUS WITH ANOTHER BOARD
  ----------------------------------
  Another board strobes this same bus, and its traffic has to be rejected. Two facts, both confirmed on the bench, make that possible:

  1. Interloper strobes only ever report address 15 (their decoder's "15" output is deliberately left unconnected). They never 
    present 0-14. So any address in 0-14 is ours and can be accepted on sight.

  2. Address 15 is therefore ambiguous - it is both our end-of-frame marker and every interloper strobe. Context resolves it: 
    a genuine end-of-frame 15 only ever arrives directly after address 14, the last data address. A 15 seen at any other moment 
    is an interloper, and is ignored.

  Interloper strobes also make their own PD lines pulse, which would otherwise look like real data for whatever address we last 
  latched. So a PD pulse is only trusted if no strobe of any kind has moved the bus since we latched: lastStrobeAddr must still 
  equal latchedAddr. If the interloper strobed in between, lastStrobeAddr changed while latchedAddr did not, and the mismatch 
  discards the pulse.

  Address 0 reliably marks a genuine frame start (confirmed it doesn't appear as noise), so it doubles as the resync point.

  ARCHITECTURE
  ------------
  Everything flows through one 64-bit lamp bitmap, published once per frame:

      decoder ISRs  --\
                       >-- publishFrame() -> takeFrame() -> FadeEngine -> TLC5947
      demo modes    --/

  The demo modes are a stand-in for the MPU: they publish frames exactly as the decoder does, so every stage downstream is 
  identical whether the lamps are being driven by a real machine or by a bench pattern. Deleting the demos for the final build 
  means deleting one section and one call.

  The five pin ISRs are the only interrupt code in the project. Fading runs on a timer gate in loop(), which is otherwise idle - 
  the fade needs to be regular, not instantaneous, and keeping it out of interrupt context is what lets it be an ordinary object 
  with no volatile state.
*/

#include "TLC5947.h"
#include "FadeEngine.h"

// Set to 0 for the final build: drops the bench demo patterns and the button that cycles them, leaving only live decode.
#define ENABLE_DEMO_MODES 1

// Set to 0 for the final build: drops the CPU load meter and its Serial output. See the LOAD METER section below.
#define ENABLE_LOAD_METER 1

// ==================================================================
// Configuration
// ==================================================================

// ---- TLC5947 LED drivers ----
#define NUM_TLC5947   3
#define NUM_CHANNELS  (NUM_TLC5947 * 24)   // 72
#define MAX_LEVEL     4095                 // TLC5947 is 12-bit

// Software (bit-banged) SPI. The prototype interface board has the data pin mis-assigned, so hardware SPI is not usable on it; 
// the driver supports both, and this becomes the hardware-SPI constructor once the redesigned board arrives. Bit-banging 72 
// channels costs ~150us per write, which at the fade tick rate below is a few percent of one core. It is not a problem.
#define TLC_CLOCK 13
#define TLC_DATA  12
#define TLC_LATCH 14
#define TLC_BLANK 15
TLC5947 tlc(NUM_TLC5947, TLC_CLOCK, TLC_DATA, TLC_LATCH, TLC_BLANK);

// ---- Inputs from the MPU ----
#define PIN_AD0    3
#define PIN_AD1    2
#define PIN_AD2    1
#define PIN_AD3    0
#define PIN_PD0    5
#define PIN_PD1    6
#define PIN_PD2    7
#define PIN_PD3    8
#define PIN_STROBE 4   // must be interrupt-capable (all Teensy 4 pins are)

// ---- Front-panel controls ----
#define PIN_DEMO_SWITCH 11   // momentary to GND -> pullup, active low
#define PIN_POT_FADE    A9   // one pot; sets fade-in, fade-out follows it

// ---- Fade behaviour ----
const uint32_t FADE_TICK_MS = 5;     // 200Hz; well above flicker perception

// The pot sets the fade-in time; fade-out is always this much slower, to mimic a filament cooling more slowly than it heats.
const uint16_t FADE_IN_MIN_MS  = 10;
const uint16_t FADE_IN_MAX_MS  = 200;
const float    FADE_OUT_RATIO  = 3.5f;   // so fade-out spans 35ms .. 700ms

const FadeCurve FADE_IN_CURVE  = FadeCurve::ExpoIn;
const FadeCurve FADE_OUT_CURVE = FadeCurve::LogOut;

FadeEngine fade(NUM_CHANNELS, MAX_LEVEL);

// ---- Control polling ----
const uint32_t POT_POLL_MS    = 250;   // set once with the backbox open, then never touched - slow is fine, and the deadband 
const uint16_t POT_DEADBAND   = 8;     // stops ADC noise wobbling the fade times between polls matches analogReadResolution(10)
const uint16_t POT_MAX        = 1023;  // matches analogReadResolution(10)
const uint32_t DEBOUNCE_MS    = 50;

// ==================================================================
// Lamp map
// ==================================================================
// Which lamp bit (pd * 16 + addr) drives which TLC5947 output channel. Several channels legitimately share a bit: some original 
// lamp drivers fed two SCRs, lighting the same signal on both the playfield and the backbox.
// LAMP_BIT_NONE marks a channel nothing drives; it stays dark.
//
// NOTE: this table has not yet been reconciled against the original board and is known to be at least partly wrong. 
// Channels 58, 68 and 71 map to bits 47, 31 and 15 - all of which are addr==15, the end-of-frame sentinel, so no PD pulse can 
// ever set them and those channels can never light. Lamp bits 60, 61 and 62 are meanwhile mapped to no channel at all. Left 
// as-is deliberately, pending a proper review against the machine.
const uint8_t LAMP_BIT_NONE = 0xFF;

const uint8_t channelToLampBit[NUM_CHANNELS] = {
  19, 18, 25, 23, 22, 21, 17, 16, 20, 24, 26, 37,  4,  5,  6,  2,  0,  1,  3,  8,  9, 10, 11,  7,
  27, 12, 10, 28, 13, 29, 14, 11, 26, 42, 57, 40, 25, 58, 43, 55, 41, 56, 44, 59,
  LAMP_BIT_NONE, LAMP_BIT_NONE, LAMP_BIT_NONE, LAMP_BIT_NONE,
  45, 52, 50, 51, 49, 53, 48, 46, 55, 56, 47, 34, 33, 54, 32, 39, 38, 40, 35, 41, 31, 30, 36, 15
};
static_assert(sizeof(channelToLampBit) == NUM_CHANNELS,
              "lamp map must have exactly one entry per TLC5947 channel");

// ==================================================================
// Frame handoff
// ==================================================================
// The one place a completed lamp bitmap is published, and the one place it is consumed. The decoder publishes from an ISR; 
// the demo modes publish from loop(). A uint64_t is not written atomically on a 32-bit core, so both ends guard the access - 
// and they do so by saving and restoring the interrupt mask rather than blindly re-enabling, which keeps them correct whether 
// the caller is inside an ISR or not.

// Which producer owns the handoff. Live is the real machine; the rest are bench patterns (see ENABLE_DEMO_MODES). 
// Exactly one publishes at a time.
enum class LampSource : uint8_t { Live, RandomFlicker, Blink, Chase };
volatile LampSource lampSource = LampSource::Live;

volatile uint64_t pendingFrame = 0;
volatile bool     frameFresh   = false;

// Save-and-restore of the interrupt mask, rather than noInterrupts() / interrupts(). The difference matters: 
// interrupts() unconditionally re-enables, so calling it from inside an ISR would drop the mask early.
// publishFrame() is called from an ISR (decode) and from loop() (demos), so it has to be correct in both.
static inline uint32_t irqSave() {
  uint32_t primask;
  __asm__ volatile("MRS %0, primask" : "=r"(primask) :: "memory");
  __asm__ volatile("CPSID i" ::: "memory");
  return primask;
}

static inline void irqRestore(uint32_t primask) {
  __asm__ volatile("MSR primask, %0" :: "r"(primask) : "memory");
}

void publishFrame(uint64_t lampBits) {
  const uint32_t mask = irqSave();
  pendingFrame = lampBits;
  frameFresh   = true;
  irqRestore(mask);
}

// Returns false if no new frame has arrived since the last call.
bool takeFrame(uint64_t &out) {
  bool got = false;
  const uint32_t mask = irqSave();
  if (frameFresh) {
    out        = pendingFrame;
    frameFresh = false;
    got        = true;
  }
  irqRestore(mask);
  return got;
}

// ==================================================================
// Decoder
// ==================================================================

const uint8_t ADDR_FRAME_START = 0;     // also the resync point
const uint8_t ADDR_LAST_DATA   = 14;    // 0..14 carry lamp data
const uint8_t ADDR_END_FRAME   = 15;    // sentinel; also every interloper strobe
const uint8_t ADDR_NONE        = 0xFF;  // no frame in progress

// Written by strobeISR, read by the PD ISRs.
volatile uint8_t latchedAddr    = ADDR_NONE;  // the address the current frame is on
volatile uint8_t lastStrobeAddr = ADDR_NONE;  // the address on the most recent strobe of
                                              // ANY kind, ours or an interloper's

// The frame being accumulated, one uint16 per PD group, one bit per address.
// Private to the ISRs; packed into a uint64 and published at end-of-frame.
volatile uint16_t pdBits[4] = {0, 0, 0, 0};

// Tracks the current frame's address. Touches no PD line.
void strobeISR() {
  const uint8_t addr = digitalReadFast(PIN_AD0)
                     | (digitalReadFast(PIN_AD1) << 1)
                     | (digitalReadFast(PIN_AD2) << 2)
                     | (digitalReadFast(PIN_AD3) << 3);

  lastStrobeAddr = addr;   // recorded whether or not we accept it below

  if (addr == ADDR_FRAME_START) {
    pdBits[0] = pdBits[1] = pdBits[2] = pdBits[3] = 0;
    latchedAddr = ADDR_FRAME_START;
    return;
  }

  if (addr == ADDR_END_FRAME) {
    // Only a genuine end-of-frame if we just finished the last data address.
    // Anything else reporting 15 is the other board on the bus.
    if (latchedAddr == ADDR_LAST_DATA) {
      // Keep decoding while a demo runs, but don't publish - 
      // the demo owns the handoff, and a live MPU would otherwise fight it for the lamps.
      if (lampSource == LampSource::Live) {
        publishFrame((uint64_t)pdBits[0]
                   | ((uint64_t)pdBits[1] << 16)
                   | ((uint64_t)pdBits[2] << 32)
                   | ((uint64_t)pdBits[3] << 48));
      }
      latchedAddr = ADDR_NONE;
    }
    return;
  }

  // Addresses 1-14 only mean anything once a frame has actually started.
  if (latchedAddr != ADDR_NONE) latchedAddr = addr;
}

// A PD line has gone low: its decoder is presenting its latched output, so the lamp at (this group, the latched address) is lit.
// Trust it only if no strobe has moved the bus since we latched that address - otherwise this pulse belongs to another board's 
// traffic, not ours.
inline void pdFallingEdge(uint8_t pd) {
  if (latchedAddr != ADDR_NONE && latchedAddr == lastStrobeAddr) {
    pdBits[pd] |= (uint16_t)1 << latchedAddr;
  }
}

void pd0ISR() { pdFallingEdge(0); }
void pd1ISR() { pdFallingEdge(1); }
void pd2ISR() { pdFallingEdge(2); }
void pd3ISR() { pdFallingEdge(3); }

// ==================================================================
// Demo modes  (bench only - see ENABLE_DEMO_MODES)
// ==================================================================
// These publish frames exactly as the decoder does, so they exercise the real lamp map and the real fade path. While a demo runs, 
// strobeISR keeps decoding but stops publishing, so a live MPU on the bus is simply ignored.

#if ENABLE_DEMO_MODES

const uint8_t LAMP_COUNT = 60;   // 15 addresses x 4 PD groups

// The 60 real lamp bit indices, in order - the demos step through these rather than through raw channel numbers, 
// so they only ever light bits the protocol could actually deliver.
uint8_t lampBitIndex[LAMP_COUNT];
uint64_t allLampsMask = 0;

void buildLampBitIndex() {
  uint8_t n = 0;
  for (uint8_t pd = 0; pd < 4; pd++) {
    for (uint8_t addr = 0; addr <= ADDR_LAST_DATA; addr++) {
      const uint8_t bit = pd * 16 + addr;
      lampBitIndex[n++] = bit;
      allLampsMask |= (uint64_t)1 << bit;
    }
  }
}

const uint32_t DEMO_FLICKER_MS = 400;   // slow enough to watch a full fade
const uint32_t DEMO_BLINK_MS   = 500;
const uint32_t DEMO_CHASE_MS   = 250;
const uint8_t  DEMO_FLICKER_ON_PERCENT = 20;   // average share of lamps lit

uint32_t demoLastChange = 0;
uint8_t  demoChaseIdx   = 0;
bool     demoBlinkOn    = false;

void serviceDemo(uint32_t now) {
  switch (lampSource) {
    case LampSource::RandomFlicker:
      if (now - demoLastChange >= DEMO_FLICKER_MS) {
        demoLastChange = now;
        uint64_t bits = 0;
        for (uint8_t i = 0; i < LAMP_COUNT; i++) {
          if (random(100) < DEMO_FLICKER_ON_PERCENT) {
            bits |= (uint64_t)1 << lampBitIndex[i];
          }
        }
        publishFrame(bits);
      }
      break;

    case LampSource::Blink:
      if (now - demoLastChange >= DEMO_BLINK_MS) {
        demoLastChange = now;
        demoBlinkOn = !demoBlinkOn;
        publishFrame(demoBlinkOn ? allLampsMask : 0);
      }
      break;

    case LampSource::Chase:
      if (now - demoLastChange >= DEMO_CHASE_MS) {
        demoLastChange = now;
        publishFrame((uint64_t)1 << lampBitIndex[demoChaseIdx]);
        demoChaseIdx = (demoChaseIdx + 1) % LAMP_COUNT;
      }
      break;

    case LampSource::Live:
      break;
  }
}

// Cycle to the next source, and publish an empty frame so the outgoing mode's pattern fades away instead of being stranded on the lamps.
void nextLampSource() {
  switch (lampSource) {
    case LampSource::Live:          lampSource = LampSource::RandomFlicker; break;
    case LampSource::RandomFlicker: lampSource = LampSource::Blink;         break;
    case LampSource::Blink:         lampSource = LampSource::Chase;         break;
    case LampSource::Chase:         lampSource = LampSource::Live;          break;
  }

  demoLastChange = 0;
  demoChaseIdx   = 0;
  demoBlinkOn    = false;
  publishFrame(0);
}

void serviceModeSwitch(uint32_t now) {
  static bool     lastState  = HIGH;
  static uint32_t lastChange = 0;

  const bool sw = digitalRead(PIN_DEMO_SWITCH);
  if (sw != lastState && (now - lastChange) > DEBOUNCE_MS) {
    lastChange = now;
    lastState  = sw;
    if (sw == LOW) nextLampSource();   // active low
  }
}

#endif  // ENABLE_DEMO_MODES

// ==================================================================
// Controls
// ==================================================================

void servicePot(uint32_t now) {
  static uint32_t lastPoll = 0;
  static int16_t  lastRaw  = -1;

  if (now - lastPoll < POT_POLL_MS) return;
  lastPoll = now;

  const int16_t raw = analogRead(PIN_POT_FADE);
  if (lastRaw >= 0) {
    const int16_t moved = raw > lastRaw ? raw - lastRaw : lastRaw - raw;
    if (moved < POT_DEADBAND) return;
  }
  lastRaw = raw;

  const uint16_t riseMs = map(raw, 0, POT_MAX, FADE_IN_MIN_MS, FADE_IN_MAX_MS);
  fade.setTimes(riseMs, (uint16_t)(riseMs * FADE_OUT_RATIO));
}

// ==================================================================
// Load meter  (bench only - see ENABLE_LOAD_METER)
// ==================================================================
// Answers "how hard is the Teensy actually working, and what is the worst-case
// delay a fade tick can suffer?" - the second being the number that matters,
// since loop()-driven fades are only smooth if no single pass runs long.
//
// Built on ARM_DWT_CYCCNT, the Cortex-M7's free-running cycle counter. It ticks
// at F_CPU_ACTUAL (600MHz here) and wraps every ~7.2 seconds, which is harmless:
// every measurement subtracts two reads taken microseconds apart, and unsigned
// subtraction wraps correctly. The counter is already enabled and proven - the
// TLC5947 driver's pulseGuard() leans on it via delayNanoseconds().
//
// WHAT "LOAD" MEANS HERE. loop() never sleeps; it spins. So the CPU is always
// executing something, and measuring "time inside loop()" would report ~100% and
// tell you nothing. Instead the meter times only the blocks that do real work
// and divides by wall-clock cycles. Idle passes cost nothing to measure because
// they aren't measured at all.
// - load — share of wall-clock cycles spent doing real work. My prediction is ~3-5%; 
//   if it reads wildly higher, suspect the meter before the firmware.
// - pass peak — the longest single fade-tick block in the last second. 
//   This is the one that matters. It's the worst-case delay a tick can suffer, and it's the direct test of 
//   the "jitter well under a millisecond against a 5 ms tick" claim. If it ever approaches 5000 µs, 
//   the loop-driven fade design is in trouble and the IntervalTimer fallback earns its keep.
// - tick and write — last/peak µs for the curve math and the bit-banged shift. 
//   write is the one to watch: ~150 µs is my estimate, never measured, and it's the number my whole architecture argument rests on.
// - frames — frames applied per second. ~120 live (AC zero-cross); in a demo mode it'll be much lower, 
//   since demos only publish on pattern changes.
//
// TWO CAVEATS, both small:
//   - An ISR that preempts a measured block has its cycles charged to that
//     block. The five pin ISRs run ~9k/sec at ~100 cycles each (well under 1%),
//     so this doesn't move the number. Note they only fire on live MPU traffic -
//     on the bench, in a demo mode, ISR cost reads as zero because it is zero.
//   - The pot poll and demo pattern generation aren't instrumented: a few
//     microseconds per second between them, far below the noise floor. Load is
//     therefore a hair under-reported.

#if ENABLE_LOAD_METER

const uint32_t LOAD_REPORT_MS = 5000;

// One instrumented block's stats for the current reporting window.
//
// add() and reset() are members rather than free functions on purpose: the
// Arduino IDE auto-generates prototypes for free functions and injects them at
// the top of the .ino, above this struct - so a free meterAccumulate(MeterStat&)
// would be prototyped before MeterStat exists and fail to compile. Members are
// never auto-prototyped, which sidesteps it entirely. Don't "simplify" these
// back into free functions.
struct MeterStat {
  uint32_t total;   // cycles accumulated
  uint32_t last;    // most recent sample
  uint32_t peak;    // largest single sample
  uint32_t count;   // samples taken

  void add(uint32_t cycles) {
    total += cycles;
    last   = cycles;
    count++;
    if (cycles > peak) peak = cycles;
  }

  void reset() { total = 0; peak = 0; count = 0; }
};

MeterStat meterWork  = {0, 0, 0, 0};   // whole fade-tick block: tick + setPWM + write
MeterStat meterTick  = {0, 0, 0, 0};   // just fade.tick() - the curve math
MeterStat meterWrite = {0, 0, 0, 0};   // just tlc.write() - the bit-banged shift
MeterStat meterFrame = {0, 0, 0, 0};   // applying a new frame to the fade targets

uint32_t meterWindowStartCycles = 0;
uint32_t meterReportMs = 0;

inline uint32_t cyclesToMicros(uint32_t cycles) { return cycles / (F_CPU_ACTUAL / 1000000UL); }

#define METER_BEGIN(name)        const uint32_t name##_t0 = ARM_DWT_CYCCNT
#define METER_END(name, stat)    (stat).add(ARM_DWT_CYCCNT - name##_t0)

void reportLoad(uint32_t now) {
  if (now - meterReportMs < LOAD_REPORT_MS) return;
  meterReportMs = now;

  const uint32_t wallCycles = ARM_DWT_CYCCNT - meterWindowStartCycles;
  meterWindowStartCycles = ARM_DWT_CYCCNT;

  // Integer math throughout - no float printf. Load in hundredths of a percent;
  // the uint64 cast matters, since 600M cycles x 10000 overflows 32 bits.
  const uint32_t busy = meterWork.total + meterFrame.total;
  const uint32_t loadHundredths = wallCycles
    ? (uint32_t)(((uint64_t)busy * 10000ULL) / wallCycles) : 0;

  // Casts are explicit because uint32_t is unsigned int on some ARM toolchains
  // and unsigned long on others; %lu plus a cast is right on both.
  Serial.printf(
    "load %2lu.%02lu%%   pass peak %4luus   tick %3lu/%3luus   write %3lu/%3luus   frames %3lu/s\n",
    (unsigned long)(loadHundredths / 100), (unsigned long)(loadHundredths % 100),
    (unsigned long)cyclesToMicros(meterWork.peak),
    (unsigned long)cyclesToMicros(meterTick.last),  (unsigned long)cyclesToMicros(meterTick.peak),
    (unsigned long)cyclesToMicros(meterWrite.last), (unsigned long)cyclesToMicros(meterWrite.peak),
    (unsigned long)meterFrame.count);

  meterWork.reset();
  meterTick.reset();
  meterWrite.reset();
  meterFrame.reset();
}

#else
#define METER_BEGIN(name)
#define METER_END(name, stat)
#endif  // ENABLE_LOAD_METER

// ==================================================================
// Setup / loop
// ==================================================================

void setup() {
  const uint8_t inputs[] = {
    PIN_AD0, PIN_AD1, PIN_AD2, PIN_AD3,
    PIN_PD0, PIN_PD1, PIN_PD2, PIN_PD3,
    PIN_STROBE, PIN_DEMO_SWITCH
  };
  for (uint8_t i = 0; i < sizeof(inputs); i++) pinMode(inputs[i], INPUT_PULLUP);

  analogReadResolution(10);   // POT_MAX assumes this

  attachInterrupt(digitalPinToInterrupt(PIN_STROBE), strobeISR, RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_PD0), pd0ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_PD1), pd1ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_PD2), pd2ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_PD3), pd3ISR, FALLING);

  fade.setCurves(FADE_IN_CURVE, FADE_OUT_CURVE);

#if ENABLE_DEMO_MODES
  buildLampBitIndex();
#endif

  // begin() latches an all-zero frame and leaves BLANK high, so the LEDs are
  // held hard-off until the first frame lights a lamp and loop() writes - which
  // is also what releases BLANK. No garbage flash at power-on, no wasted write.
  tlc.begin();

#if ENABLE_LOAD_METER
  // Deliberately no `while (!Serial)` - this board has to run standalone in a
  // machine, and waiting for a USB host that will never arrive would hang it.
  Serial.begin(115200);
  meterWindowStartCycles = ARM_DWT_CYCCNT;
#endif
}

void loop() {
  static uint32_t lastFadeMs = 0;
  const uint32_t now = millis();

#if ENABLE_DEMO_MODES
  serviceDemo(now);
  serviceModeSwitch(now);
#endif

  // A frame from whichever source is live becomes the fade engine's targets.
  uint64_t lampBits;
  if (takeFrame(lampBits)) {
    METER_BEGIN(frame);
    for (uint16_t ch = 0; ch < NUM_CHANNELS; ch++) {
      const uint8_t bit = channelToLampBit[ch];
      if (bit == LAMP_BIT_NONE) continue;
      fade.setTarget(ch, ((lampBits >> bit) & 1ULL) != 0, now);
    }
    METER_END(frame, meterFrame);
  }

  // Advance the fades on a regular tick. loop() has nothing else to do and its slowest pass is tlc.write() at ~150us,
  // so the jitter against a 5ms tick is well under a millisecond - far below anything the eye resolves.
  // (ENABLE_LOAD_METER measures that claim rather than trusting it: "pass peak" is the worst-case delay a tick can suffer.)
  if (now - lastFadeMs >= FADE_TICK_MS) {
    METER_BEGIN(work);
    lastFadeMs = now;

    METER_BEGIN(tick);
    fade.tick(now);
    METER_END(tick, meterTick);

    if (fade.dirty()) {
      for (uint16_t ch = 0; ch < NUM_CHANNELS; ch++) tlc.setPWM(ch, fade.level(ch));
      METER_BEGIN(write);
      tlc.write();
      METER_END(write, meterWrite);
      fade.clearDirty();
    }
    METER_END(work, meterWork);
  }

  servicePot(now);

#if ENABLE_LOAD_METER
  reportLoad(now);
#endif
}
