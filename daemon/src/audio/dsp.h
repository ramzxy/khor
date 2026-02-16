#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace khor::dsp {

struct Adsr {
  float a_s = 0.005f;
  float d_s = 0.080f;
  float s_level = 0.55f;
  float r_s = 0.140f;

  enum Stage : uint8_t { Off = 0, Attack, Decay, Sustain, Release } stage = Off;
  float value = 0.0f;
  float release_step = 0.0f;

  void note_on(float sr) {
    (void)sr;
    stage = Attack;
    value = 0.0f;
    release_step = 0.0f;
  }

  void note_off(float sr) {
    if (stage == Off || stage == Release) return;
    stage = Release;
    const float steps = std::max(1.0f, r_s * sr);
    release_step = value / steps;
  }

  float tick(float sr) {
    const float eps = 1e-6f;
    switch (stage) {
      case Off: value = 0.0f; break;
      case Attack: {
        const float steps = std::max(1.0f, a_s * sr);
        value += 1.0f / steps;
        if (value >= 1.0f) { value = 1.0f; stage = Decay; }
      } break;
      case Decay: {
        const float steps = std::max(1.0f, d_s * sr);
        value -= (1.0f - s_level) / steps;
        if (value <= s_level) { value = s_level; stage = Sustain; }
      } break;
      case Sustain: break;
      case Release: {
        value -= release_step > 0.0f ? release_step : (1.0f / std::max(1.0f, r_s * sr));
        if (value <= eps) { value = 0.0f; stage = Off; }
      } break;
    }
    return value;
  }
};

// TPT State Variable Filter (low-pass output).
struct Svf {
  float ic1eq = 0.0f;
  float ic2eq = 0.0f;

  float process(float in, float g, float k) {
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    const float v3 = in - ic2eq;
    const float v1 = a1 * ic1eq + a2 * v3;
    const float v2 = ic2eq + a2 * ic1eq + a3 * v3;

    ic1eq = 2.0f * v1 - ic1eq;
    ic2eq = 2.0f * v2 - ic2eq;

    return v2;
  }
};

inline float midi_to_hz(int midi) {
  return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
}

} // namespace khor::dsp

