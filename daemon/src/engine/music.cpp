#include "engine/music.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace khor {
namespace {

static float clamp01f(float v) { return std::clamp(v, 0.0f, 1.0f); }
static double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

struct ScaleDef {
  const int* degrees = nullptr;
  int count = 0;
};

static ScaleDef scale_from_string(const std::string& s) {
  static constexpr int kPentaMinor[] = {0, 3, 5, 7, 10};
  static constexpr int kNatMinor[] = {0, 2, 3, 5, 7, 8, 10};
  static constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10};

  auto eq = [&](const char* name) -> bool {
    return s == name;
  };

  if (eq("pentatonic_minor") || eq("penta_minor") || eq("pentatonic")) return {kPentaMinor, (int)(sizeof(kPentaMinor) / sizeof(kPentaMinor[0]))};
  if (eq("natural_minor") || eq("minor")) return {kNatMinor, (int)(sizeof(kNatMinor) / sizeof(kNatMinor[0]))};
  if (eq("dorian")) return {kDorian, (int)(sizeof(kDorian) / sizeof(kDorian[0]))};
  return {kPentaMinor, (int)(sizeof(kPentaMinor) / sizeof(kPentaMinor[0]))};
}

static uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static double frand01(uint64_t& state) {
  uint64_t v = splitmix64(state);
  // 53 bits of mantissa.
  return (double)(v >> 11) * (1.0 / 9007199254740992.0);
}

static int pick_note(int key_midi, const ScaleDef& sc, int degree, int octave) {
  if (sc.count <= 0) return std::clamp(key_midi, 0, 127);
  degree %= sc.count;
  if (degree < 0) degree += sc.count;
  int midi = key_midi + sc.degrees[degree] + octave * 12;
  return std::clamp(midi, 0, 127);
}

static void push_note(std::vector<NoteEvent>& out, int midi, float vel, float dur_s) {
  NoteEvent ev;
  ev.midi = std::clamp(midi, 0, 127);
  ev.velocity = clamp01f(vel);
  ev.dur_s = std::max(0.02f, dur_s);
  out.push_back(ev);
}

} // namespace

MusicFrame MusicEngine::tick(const Signal01& s_in, const MusicConfig& cfg_in) {
  Signal01 s = s_in;
  MusicConfig cfg = cfg_in;

  cfg.density = clamp01(cfg.density);
  if (!(cfg.bpm > 1.0 && cfg.bpm < 400.0)) cfg.bpm = 110.0;
  cfg.key_midi = std::clamp(cfg.key_midi, 0, 127);

  const ScaleDef sc = scale_from_string(cfg.scale);
  const double activity = std::max({s.exec, s.rx, s.tx, s.csw, s.io});

  MusicFrame out;
  out.notes.reserve(8);

  // Synth params: map IO to cutoff; map exec to resonance; presets adjust FX.
  SynthParams sp;
  sp.cutoff01 = (float)clamp01(0.30 + 0.60 * s.io + 0.15 * (s.rx + s.tx) * 0.5);
  sp.resonance01 = (float)clamp01(0.18 + 0.55 * s.exec);

  const bool silent_by_default = (cfg.preset != "drone");
  if (silent_by_default && activity < 0.03) {
    // Still advance the clock, but don't emit anything.
    step_ = (step_ + 1) & 15;
    if (step_ == 0) bar_++;
    out.synth = sp;
    return out;
  }

  // Deterministic randomness seeded by the current grid position + signals.
  uint64_t seed = 0x6a09e667f3bcc909ULL;
  seed ^= (uint64_t)bar_ * 0x9e3779b97f4a7c15ULL;
  seed ^= (uint64_t)step_ * 0xbf58476d1ce4e5b9ULL;
  seed ^= (uint64_t)std::llround(s.exec * 1000000.0) * 0x94d049bb133111ebULL;
  seed ^= (uint64_t)std::llround(s.rx * 1000000.0) * 0x2545f4914f6cdd1dULL;
  seed ^= (uint64_t)std::llround(s.tx * 1000000.0) * 0x7f4a7c159e3779b9ULL;
  seed ^= (uint64_t)std::llround(s.csw * 1000000.0) * 0x1ce4e5b9bf58476dULL;
  seed ^= (uint64_t)std::llround(s.io * 1000000.0) * 0x133111eb94d049bbULL;

  const double dens = cfg.density;

  if (cfg.preset == "ambient") {
    sp.reverb_mix01 = (float)clamp01(0.38 + 0.35 * s.rx);
    sp.delay_mix01 = (float)clamp01(0.10 + 0.22 * s.tx);

    const double p_note = dens * (0.12 + 0.88 * activity) * 0.35;
    if (frand01(seed) < p_note) {
      const int deg = (int)(frand01(seed) * sc.count);
      const int oct = (int)(frand01(seed) * 3.0); // 0..2
      const int midi = pick_note(cfg.key_midi, sc, deg, oct);
      const float vel = (float)clamp01(0.12 + 0.70 * (0.65 * s.rx + 0.35 * s.tx));
      const float dur = (float)std::clamp(0.20 + 0.70 * (0.40 + 0.60 * s.rx) * (0.30 + 0.70 * dens), 0.10, 1.10);
      push_note(out.notes, midi, vel, dur);
    }

    // Exec accents: gentle dyads.
    const double p_exec = dens * s.exec * 0.18;
    if (frand01(seed) < p_exec) {
      const int root = pick_note(cfg.key_midi, sc, 0, 1);
      const int fifth = pick_note(cfg.key_midi, sc, 2, 1); // in pentatonic this is close to a fifth-ish feel
      push_note(out.notes, root, 0.42f, 0.35f);
      push_note(out.notes, fifth, 0.30f, 0.35f);
    }
  } else if (cfg.preset == "percussive") {
    sp.cutoff01 = (float)clamp01(0.62 + 0.30 * s.io);
    sp.reverb_mix01 = (float)clamp01(0.10 + 0.15 * s.rx);
    sp.delay_mix01 = (float)clamp01(0.06 + 0.10 * s.tx);

    // Kick-like low note on downbeats influenced by exec.
    if (step_ % 4 == 0) {
      const double p_kick = dens * (0.05 + 0.95 * s.exec) * 0.65;
      if (frand01(seed) < p_kick) {
        const int midi = std::clamp(cfg.key_midi - 24, 0, 127);
        push_note(out.notes, midi, (float)clamp01(0.35 + 0.55 * s.exec), 0.08f);
      }
    }

    // Clicks from scheduler activity.
    const double p_click = dens * (0.10 + 0.90 * s.csw) * 0.95;
    if (frand01(seed) < p_click) {
      const int deg = (int)(frand01(seed) * sc.count);
      const int midi = pick_note(cfg.key_midi, sc, deg, 3 + (step_ & 1)); // high
      push_note(out.notes, midi, (float)clamp01(0.18 + 0.75 * s.csw), 0.05f);
    }

    // Network adds mid hits.
    const double p_mid = dens * (0.10 + 0.90 * (s.rx + s.tx) * 0.5) * 0.35;
    if (frand01(seed) < p_mid) {
      const int deg = (int)(frand01(seed) * sc.count);
      const int midi = pick_note(cfg.key_midi, sc, deg, 2);
      push_note(out.notes, midi, (float)clamp01(0.10 + 0.60 * (s.rx + s.tx) * 0.5), 0.07f);
    }
  } else if (cfg.preset == "arp") {
    sp.reverb_mix01 = (float)clamp01(0.18 + 0.20 * s.rx);
    sp.delay_mix01 = (float)clamp01(0.22 + 0.35 * s.tx);

    static constexpr int kPattern[] = {0, 1, 2, 1};
    const int pdeg = kPattern[step_ & 3];
    const double gate = (s.rx + s.tx) * 0.5;
    const double p_arp = dens * (0.20 + 0.80 * gate);
    if (gate > 0.05 && frand01(seed) < p_arp) {
      const int midi = pick_note(cfg.key_midi, sc, pdeg, 2 + ((step_ >> 2) & 1));
      const float vel = (float)clamp01(0.12 + 0.75 * gate);
      push_note(out.notes, midi, vel, 0.12f);
    }

    // Exec adds chord stabs on bar start.
    if (step_ == 0) {
      const double p_stab = dens * (0.10 + 0.90 * s.exec) * 0.6;
      if (frand01(seed) < p_stab) {
        const int root = pick_note(cfg.key_midi, sc, 0, 1);
        const int up = pick_note(cfg.key_midi, sc, 2, 1);
        push_note(out.notes, root, 0.45f, 0.20f);
        push_note(out.notes, up, 0.30f, 0.20f);
      }
    }
  } else { // drone
    sp.reverb_mix01 = (float)clamp01(0.45 + 0.25 * s.rx);
    sp.delay_mix01 = (float)clamp01(0.05 + 0.10 * s.tx);
    sp.cutoff01 = (float)clamp01(0.18 + 0.78 * s.io);
    sp.resonance01 = (float)clamp01(0.30 + 0.55 * s.exec);

    // Sustain a low root by retriggering each bar.
    if (step_ == 0) {
      const int midi = std::clamp(cfg.key_midi - 24, 0, 127);
      push_note(out.notes, midi, (float)clamp01(0.08 + 0.28 * s.io), 2.3f);
    }
    if (step_ == 8 && activity > 0.10) {
      const int midi = std::clamp(cfg.key_midi - 12, 0, 127);
      push_note(out.notes, midi, (float)clamp01(0.05 + 0.20 * activity), 1.6f);
    }

    // Network sprinkles.
    const double p_top = dens * (0.05 + 0.95 * (s.rx + s.tx) * 0.5) * 0.25;
    if (frand01(seed) < p_top) {
      const int deg = (int)(frand01(seed) * sc.count);
      const int midi = pick_note(cfg.key_midi, sc, deg, 3);
      push_note(out.notes, midi, (float)clamp01(0.05 + 0.35 * (s.rx + s.tx) * 0.5), 0.40f);
    }
  }

  out.synth = sp;

  step_ = (step_ + 1) & 15;
  if (step_ == 0) bar_++;

  return out;
}

} // namespace khor

