/*
  FadeEngine - drives a bank of channels smoothly between off and full.

  Each channel has a boolean target (on/off). The engine remembers where a
  channel was when its target last flipped, and interpolates from there to
  the new target along a curve, over a configurable duration. Rising and
  falling transitions get their own curve and their own duration, because
  an incandescent lamp warms up faster than it cools down.

  Deliberately knows nothing about pinball, lamp matrices, or LED drivers.
  It is told "channel 7 is now on"; it produces "channel 7 is currently at
  level 2043". Somebody else decides what a channel means and where the
  levels get written.

  CURVES AND THE LUT SEAM
  -----------------------
  Curves are defined mathematically (see applyCurve in the .cpp) so that
  new ones can be tried by editing one expression, rather than by
  regenerating a table. Every curve is evaluated through that single
  function, so converting to precomputed lookup tables later - for speed,
  or to allow curves to be swapped at runtime from a front-panel control -
  means rewriting applyCurve alone. Nothing else in the codebase needs to
  know it happened.
*/
#ifndef FADE_ENGINE_H
#define FADE_ENGINE_H

#include <Arduino.h>

// Curve shapes, normalized: each maps t in [0,1] to a progress fraction in
// [0,1], where t is how far through the transition's duration we are.
//   Linear    - constant rate; what this project used originally.
//   ExpoIn    - creeps, then rushes. Good for a filament coming up to heat.
//   LogOut    - drops fast, then lingers. Good for a filament cooling.
//   EaseInOut - symmetric S-curve; soft at both ends.
enum class FadeCurve : uint8_t { Linear, ExpoIn, LogOut, EaseInOut };

class FadeEngine {
public:
  explicit FadeEngine(uint16_t numChannels, uint16_t maxLevel = 4095);
  ~FadeEngine();

  // Curve and duration for rising (off->on) and falling (on->off)
  // transitions. Durations are the time for a full-swing transition; a
  // channel interrupted mid-fade still uses the full duration, so it moves
  // proportionally faster over the shorter distance it has left to cover.
  // A duration of 0 snaps instantly.
  void setCurves(FadeCurve rising, FadeCurve falling);
  void setTimes(uint16_t riseMs, uint16_t fallMs);

  // Set a channel's target. If this flips the target, the channel's current
  // level becomes the starting point of a new transition beginning at nowMs.
  // Calling it with an unchanged target is free.
  void setTarget(uint16_t ch, bool on, uint32_t nowMs);

  // Advance every in-flight channel to where it should be at nowMs.
  // Channels already sitting at their target cost nothing.
  void tick(uint32_t nowMs);

  uint16_t level(uint16_t ch) const { return ch < channelCount ? levels[ch] : 0; }
  uint16_t channels() const { return channelCount; }

  // True if any level actually changed since the last clearDirty(), i.e.
  // there is something new worth shifting out to the hardware.
  bool dirty() const { return levelsDirty; }
  void clearDirty() { levelsDirty = false; }

private:
  static float applyCurve(FadeCurve curve, float t);

  uint16_t channelCount;
  uint16_t maxLevel;

  uint16_t *levels;      // where each channel is right now
  uint16_t *fromLevel;   // where it was when its current transition began
  uint32_t *startMs;     // when that transition began
  bool     *targetOn;    // where it is heading

  FadeCurve riseCurve;
  FadeCurve fallCurve;
  uint16_t  riseMs;
  uint16_t  fallMs;
  bool      levelsDirty;
};

#endif
