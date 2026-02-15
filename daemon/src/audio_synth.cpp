#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <mutex>
#include <vector>

#include "miniaudio.h"

#include "khor/audio.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kMaxVoices = 16;

static double midi_to_hz(int midi) {
  return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

struct Voice {
  bool active = false;
  int midi = 0;
  double phase = 0.0;
  double phase_inc = 0.0;
  double env = 0.0;
  double env_inc = 0.0;
  int samples_left = 0;
  float velocity = 0.5f;
};

struct SynthState {
  std::mutex mu;
  std::array<Voice, kMaxVoices> voices;
  float master = 0.25f;
};

SynthState g;

void audio_cb(ma_device* device, void* out, const void* in, ma_uint32 frames) {
  (void)in;
  (void)device;
  float* out_f = (float*)out;

  // Interleaved stereo float32.
  std::fill(out_f, out_f + frames * 2, 0.0f);

  std::scoped_lock lk(g.mu);
  for (auto& v : g.voices) {
    if (!v.active) continue;
    for (ma_uint32 i = 0; i < frames; i++) {
      if (v.samples_left <= 0) {
        v.active = false;
        break;
      }

      // Simple pleasant-ish tone: sine + quiet second harmonic.
      double s = std::sin(v.phase) + 0.15 * std::sin(2.0 * v.phase);
      v.phase += v.phase_inc;
      const double two_pi = 2.0 * std::numbers::pi;
      if (v.phase > two_pi) v.phase -= two_pi;

      // Linear decay envelope (MVP). Replace with ADSR later.
      v.env = std::max(0.0, v.env - v.env_inc);
      float sample = (float)(s * v.env) * v.velocity * g.master;

      out_f[i * 2 + 0] += sample;
      out_f[i * 2 + 1] += sample;

      v.samples_left--;
    }
  }

  // Soft clip to avoid harsh overload in MVP.
  for (ma_uint32 i = 0; i < frames * 2; i++) {
    float x = out_f[i];
    out_f[i] = x / (1.0f + std::abs(x));
  }
}

} // namespace

struct KhorAudio {
  ma_context ctx{};
  bool ctx_inited = false;
  ma_device device{};
};

static bool streq_ci(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool parse_backend_override(const char* s, ma_backend* out) {
  if (!s || !out) return false;
  if (streq_ci(s, "pulseaudio") || streq_ci(s, "pulse")) { *out = ma_backend_pulseaudio; return true; }
  if (streq_ci(s, "alsa")) { *out = ma_backend_alsa; return true; }
  if (streq_ci(s, "null")) { *out = ma_backend_null; return true; }
  return false;
}

static bool audio_start(KhorAudio* a) {
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = (ma_uint32)kSampleRate;
  cfg.dataCallback = audio_cb;

  const char* backend_env = std::getenv("KHOR_AUDIO_BACKEND"); // pulseaudio|alsa|null
  const bool allow_null = (std::getenv("KHOR_AUDIO_ALLOW_NULL") != nullptr);

  ma_backend backends[3]{};
  ma_uint32 backend_count = 0;

  ma_backend forced{};
  if (parse_backend_override(backend_env, &forced)) {
    backends[backend_count++] = forced;
  } else {
    backends[backend_count++] = ma_backend_pulseaudio;
    backends[backend_count++] = ma_backend_alsa;
  }
  if (allow_null) backends[backend_count++] = ma_backend_null;

  if (ma_context_init(backends, backend_count, nullptr, &a->ctx) != MA_SUCCESS) return false;
  a->ctx_inited = true;

  if (ma_device_init(&a->ctx, &cfg, &a->device) != MA_SUCCESS) return false;
  if (ma_device_start(&a->device) != MA_SUCCESS) return false;

  const char* backend_name = "unknown";
  if (a->device.pContext) backend_name = ma_get_backend_name(a->device.pContext->backend);
  std::fprintf(stderr, "khor-audio: backend=%s\n", backend_name);
  return true;
}

static void audio_stop(KhorAudio* a) {
  ma_device_uninit(&a->device);
  if (a->ctx_inited) ma_context_uninit(&a->ctx);
  a->ctx_inited = false;
}

static void audio_note_on(KhorAudio*, int midi, float velocity, double seconds) {
  const double hz = midi_to_hz(midi);
  std::scoped_lock lk(g.mu);

  // Find a free voice, or steal voice 0 (MVP).
  Voice* slot = nullptr;
  for (auto& v : g.voices) {
    if (!v.active) { slot = &v; break; }
  }
  if (!slot) slot = &g.voices[0];

  slot->active = true;
  slot->midi = midi;
  slot->phase = 0.0;
  slot->phase_inc = 2.0 * std::numbers::pi * hz / kSampleRate;
  slot->velocity = std::clamp(velocity, 0.0f, 1.0f);
  slot->samples_left = (int)(seconds * kSampleRate);
  slot->env = 1.0;
  slot->env_inc = 1.0 / std::max(1, slot->samples_left);
}

extern "C" {

KhorAudio* khor_audio_create(void) { return new KhorAudio(); }
void khor_audio_destroy(KhorAudio* a) { delete a; }

int khor_audio_start(KhorAudio* a) { return a && audio_start(a); }
void khor_audio_stop(KhorAudio* a) { if (a) audio_stop(a); }

void khor_audio_note_on(KhorAudio* a, int midi, float velocity, double seconds) {
  if (!a) return;
  audio_note_on(a, midi, velocity, seconds);
}

} // extern "C"
