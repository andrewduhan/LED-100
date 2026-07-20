#include "FadeEngine.h"
#include <math.h>

FadeEngine::FadeEngine(uint16_t numChannels, uint16_t maxLevelIn)
  : channelCount(numChannels), maxLevel(maxLevelIn),
    riseCurve(FadeCurve::ExpoIn), fallCurve(FadeCurve::LogOut),
    riseMs(100), fallMs(350), levelsDirty(false) {
  levels    = new uint16_t[channelCount]();
  fromLevel = new uint16_t[channelCount]();
  startMs   = new uint32_t[channelCount]();
  targetOn  = new bool[channelCount]();
}

FadeEngine::~FadeEngine() {
  delete[] levels;
  delete[] fromLevel;
  delete[] startMs;
  delete[] targetOn;
}

void FadeEngine::setCurves(FadeCurve rising, FadeCurve falling) {
  riseCurve = rising;
  fallCurve = falling;
}

void FadeEngine::setTimes(uint16_t riseMsIn, uint16_t fallMsIn) {
  riseMs = riseMsIn;
  fallMs = fallMsIn;
}

void FadeEngine::setTarget(uint16_t ch, bool on, uint32_t nowMs) {
  if (ch >= channelCount || targetOn[ch] == on) return;

  // Start the new transition from wherever this channel happens to be,
  // which may be part-way through the transition it is now abandoning.
  fromLevel[ch] = levels[ch];
  startMs[ch]   = nowMs;
  targetOn[ch]  = on;
}

void FadeEngine::tick(uint32_t nowMs) {
  for (uint16_t ch = 0; ch < channelCount; ch++) {
    const uint16_t target = targetOn[ch] ? maxLevel : 0;
    if (levels[ch] == target) continue;   // settled; nothing to do

    const uint16_t durMs   = targetOn[ch] ? riseMs : fallMs;
    const uint32_t elapsed = nowMs - startMs[ch];

    uint16_t next;
    if (durMs == 0 || elapsed >= durMs) {
      next = target;
    } else {
      const float progress = applyCurve(targetOn[ch] ? riseCurve : fallCurve,
                                        (float)elapsed / (float)durMs);
      const int32_t span = (int32_t)target - (int32_t)fromLevel[ch];
      next = (uint16_t)((int32_t)fromLevel[ch] + (int32_t)(span * progress));
    }

    if (next != levels[ch]) {
      levels[ch]  = next;
      levelsDirty = true;
    }
  }
}

// Maps t (fraction of the transition's duration elapsed) to a fraction of
// the transition's distance covered. Both are in [0,1].
//
// This is the single point every fade passes through. Replacing these
// expressions with precomputed lookup tables is a change to this function
// and to nothing else.
float FadeEngine::applyCurve(FadeCurve curve, float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;

  switch (curve) {
    case FadeCurve::ExpoIn:    return powf(2.0f, 10.0f * (t - 1.0f));
    case FadeCurve::LogOut:    return log10f(1.0f + 9.0f * t);
    case FadeCurve::EaseInOut: return (1.0f - cosf((float)PI * t)) * 0.5f;
    case FadeCurve::Linear:
    default:                   return t;
  }
}
