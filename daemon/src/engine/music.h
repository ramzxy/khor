#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/note_event.h"
#include "engine/signals.h"

namespace khor {

struct MusicConfig {
  double bpm = 110.0;
  int key_midi = 62; // D4
  std::string scale = "pentatonic_minor";
  std::string preset = "ambient"; // ambient|percussive|arp|drone
  double density = 0.35;          // 0..1
};

struct SynthParams {
  float cutoff01 = 0.65f;
  float resonance01 = 0.25f;
  float delay_mix01 = 0.10f;
  float reverb_mix01 = 0.15f;
};

struct MusicFrame {
  std::vector<NoteEvent> notes;
  SynthParams synth;
};

// Deterministic 16th-note sequencer driven by Signal01.
class MusicEngine {
 public:
  MusicFrame tick(const Signal01& s, const MusicConfig& cfg);

  // For scheduling the next tick.
  static double tick_ms(double bpm) {
    // 16th note grid.
    if (!(bpm > 1.0 && bpm < 400.0)) bpm = 110.0;
    double ms = 60000.0 / bpm / 4.0;
    if (ms < 25.0) ms = 25.0;
    if (ms > 500.0) ms = 500.0;
    return ms;
  }

 private:
  uint64_t bar_ = 0;
  uint32_t step_ = 0; // 0..15
};

} // namespace khor

