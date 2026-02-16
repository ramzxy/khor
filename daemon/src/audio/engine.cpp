#include "audio/engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>

#include "miniaudio.h"

#include "audio/dsp.h"
#include "util/spsc_queue.h"

namespace khor {
namespace {

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

static bool parse_backend(const std::string& s, ma_backend* out) {
  if (!out) return false;
  if (s.empty()) return false;
  if (streq_ci(s.c_str(), "pulseaudio") || streq_ci(s.c_str(), "pulse")) { *out = ma_backend_pulseaudio; return true; }
  if (streq_ci(s.c_str(), "alsa")) { *out = ma_backend_alsa; return true; }
  if (streq_ci(s.c_str(), "null")) { *out = ma_backend_null; return true; }
  return false;
}

static std::string hex_encode(const void* data, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  const auto* p = (const unsigned char*)data;
  for (std::size_t i = 0; i < n; i++) {
    out[i * 2 + 0] = kHex[(p[i] >> 4) & 0xF];
    out[i * 2 + 1] = kHex[(p[i] >> 0) & 0xF];
  }
  return out;
}

static std::optional<std::vector<unsigned char>> hex_decode(const std::string& s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  if (s.size() % 2 != 0) return std::nullopt;
  std::vector<unsigned char> out;
  out.resize(s.size() / 2);
  for (std::size_t i = 0; i < out.size(); i++) {
    int hi = hex(s[i * 2 + 0]);
    int lo = hex(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = (unsigned char)((hi << 4) | lo);
  }
  return out;
}

static bool icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  auto lower = [](unsigned char c) -> unsigned char {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    return c;
  };

  for (std::size_t i = 0; i + needle.size() <= hay.size(); i++) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); j++) {
      if (lower((unsigned char)hay[i + j]) != lower((unsigned char)needle[j])) { ok = false; break; }
    }
    if (ok) return true;
  }
  return false;
}

struct Voice {
  bool active = false;
  int midi = 0;
  float phase = 0.0f;
  float phase_inc = 0.0f;
  float velocity = 0.7f;
  int samples_until_release = 0;
  dsp::Adsr env{};
  dsp::Svf filter{};
};

struct Delay {
  std::vector<float> buf_l;
  std::vector<float> buf_r;
  uint32_t idx = 0;
  uint32_t delay_samp = 0;
  float feedback = 0.25f;

  void init(uint32_t sr, float delay_s, float fb) {
    feedback = std::clamp(fb, 0.0f, 0.95f);
    const uint32_t max_samp = sr * 2; // 2 seconds max.
    buf_l.assign(max_samp, 0.0f);
    buf_r.assign(max_samp, 0.0f);
    idx = 0;
    delay_samp = std::clamp((uint32_t)(delay_s * (float)sr), 1u, max_samp - 1u);
  }

  void process(float& l, float& r) {
    if (buf_l.empty()) return;
    const uint32_t n = (uint32_t)buf_l.size();
    const uint32_t read = (idx + n - delay_samp) % n;

    float dl = buf_l[read];
    float dr = buf_r[read];

    buf_l[idx] = l + dl * feedback;
    buf_r[idx] = r + dr * feedback;

    idx++;
    if (idx >= n) idx = 0;

    l = dl;
    r = dr;
  }
};

struct Comb {
  std::vector<float> buf;
  uint32_t idx = 0;
  float feedback = 0.78f;
  float damp1 = 0.2f;
  float damp2 = 0.8f;
  float filterstore = 0.0f;

  void init(uint32_t n) {
    buf.assign(n, 0.0f);
    idx = 0;
    filterstore = 0.0f;
  }

  float process(float input) {
    if (buf.empty()) return 0.0f;
    float output = buf[idx];
    filterstore = (output * damp2) + (filterstore * damp1);
    buf[idx] = input + (filterstore * feedback);
    idx++;
    if (idx >= buf.size()) idx = 0;
    return output;
  }
};

struct Allpass {
  std::vector<float> buf;
  uint32_t idx = 0;
  float feedback = 0.5f;

  void init(uint32_t n) {
    buf.assign(n, 0.0f);
    idx = 0;
  }

  float process(float input) {
    if (buf.empty()) return input;
    float bufout = buf[idx];
    float output = -input + bufout;
    buf[idx] = input + (bufout * feedback);
    idx++;
    if (idx >= buf.size()) idx = 0;
    return output;
  }
};

struct Reverb {
  std::array<Comb, 4> comb_l{};
  std::array<Comb, 4> comb_r{};
  std::array<Allpass, 2> ap_l{};
  std::array<Allpass, 2> ap_r{};

  void init(uint32_t sr) {
    const float scale = (float)sr / 44100.0f;
    const auto sc = [&](int v) -> uint32_t { return (uint32_t)std::max(16, (int)std::round(v * scale)); };

    // A small-ish Freeverb-inspired network (reduced size).
    const uint32_t comb_sizes_l[4] = { sc(1116), sc(1188), sc(1277), sc(1356) };
    const uint32_t comb_sizes_r[4] = { sc(1116 + 23), sc(1188 + 23), sc(1277 + 23), sc(1356 + 23) };
    for (int i = 0; i < 4; i++) {
      comb_l[i].init(comb_sizes_l[i]);
      comb_r[i].init(comb_sizes_r[i]);
      comb_l[i].feedback = 0.78f;
      comb_r[i].feedback = 0.78f;
      comb_l[i].damp1 = 0.22f;
      comb_r[i].damp1 = 0.22f;
      comb_l[i].damp2 = 1.0f - comb_l[i].damp1;
      comb_r[i].damp2 = 1.0f - comb_r[i].damp1;
    }

    ap_l[0].init(sc(556));
    ap_l[1].init(sc(441));
    ap_r[0].init(sc(556 + 23));
    ap_r[1].init(sc(441 + 23));
  }

  void process(float& l, float& r) {
    float acc_l = 0.0f;
    float acc_r = 0.0f;
    for (int i = 0; i < 4; i++) {
      acc_l += comb_l[i].process(l);
      acc_r += comb_r[i].process(r);
    }
    // Normalize comb sum.
    acc_l *= 0.25f;
    acc_r *= 0.25f;
    for (int i = 0; i < 2; i++) {
      acc_l = ap_l[i].process(acc_l);
      acc_r = ap_r[i].process(acc_r);
    }
    l = acc_l;
    r = acc_r;
  }
};

} // namespace

struct AudioEngine::Impl {
  ma_context ctx{};
  bool ctx_inited = false;
  ma_device device{};
  std::atomic<bool> device_inited{false};
  ma_device_id chosen_playback_id{};
  bool has_chosen_playback_id = false;

  ma_backend backends[3]{};
  ma_uint32 backend_count = 0;

  AudioConfig cfg{};
  std::string backend_name;
  std::string device_name;

  SpscQueue<NoteEvent, 1024> q{};
  std::atomic<uint64_t> q_drops{0};

  static constexpr int kMaxVoices = 24;
  std::array<Voice, kMaxVoices> voices{};

  std::atomic<float> master_gain{0.25f};
  std::atomic<float> cutoff01{0.65f};
  std::atomic<float> resonance01{0.25f};
  std::atomic<float> delay_mix01{0.10f};
  std::atomic<float> reverb_mix01{0.15f};

  Delay delay{};
  Reverb reverb{};

  float limiter_gain = 1.0f;

  bool init_context(std::string* err) {
    if (ctx_inited) return true;
    ma_result r = ma_context_init(backends, backend_count, nullptr, &ctx);
    if (r != MA_SUCCESS) {
      if (err) *err = "ma_context_init failed";
      return false;
    }
    ctx_inited = true;
    return true;
  }

  void uninit_context() {
    if (ctx_inited) ma_context_uninit(&ctx);
    ctx_inited = false;
  }

  static void data_cb(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    (void)in;
    auto* self = (Impl*)device->pUserData;
    if (!self) return;
    self->render((float*)out, frames);
  }

  void render(float* out, ma_uint32 frames) {
    // Interleaved stereo f32.
    const uint32_t sr = (uint32_t)cfg.sample_rate;
    std::fill(out, out + frames * 2, 0.0f);

    // Drain note queue (SPSC, no locks).
    NoteEvent ev;
    while (q.pop(&ev)) {
      ev.midi = std::clamp(ev.midi, 0, 127);
      ev.velocity = std::clamp(ev.velocity, 0.0f, 1.0f);
      ev.dur_s = std::max(0.01f, ev.dur_s);

      // Find a free voice; otherwise steal the quietest.
      Voice* slot = nullptr;
      for (auto& v : voices) {
        if (!v.active) { slot = &v; break; }
      }
      if (!slot) {
        slot = &voices[0];
        float best = 1e9f;
        for (auto& v : voices) {
          float score = v.env.value;
          if (score < best) { best = score; slot = &v; }
        }
      }

      const float hz = dsp::midi_to_hz(ev.midi);
      slot->active = true;
      slot->midi = ev.midi;
      slot->phase = 0.0f;
      slot->phase_inc = 2.0f * (float)std::numbers::pi * hz / (float)sr;
      slot->velocity = ev.velocity;
      slot->samples_until_release = (int)(ev.dur_s * (float)sr);
      slot->env.note_on((float)sr);
      slot->filter = dsp::Svf{};
    }

    const float cutoff = std::clamp(cutoff01.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float res = std::clamp(resonance01.load(std::memory_order_relaxed), 0.0f, 1.0f);

    // Exponential cutoff mapping: ~80Hz .. ~9kHz.
    const float fc = 80.0f * std::pow(2.0f, cutoff * 6.8f);
    const float g = std::tan((float)std::numbers::pi * (fc / (float)sr));
    const float q = 0.55f + (1.0f - res) * 7.0f; // higher res => lower q mapping? keep stable
    const float k = 1.0f / std::max(0.3f, q);

    for (ma_uint32 i = 0; i < frames; i++) {
      float l = 0.0f;
      float r = 0.0f;

      for (auto& v : voices) {
        if (!v.active) continue;

        // Oscillator: sine + a little tri-ish.
        float s = std::sin(v.phase);
        float tri = (2.0f / (float)std::numbers::pi) * std::asin(std::sin(v.phase));
        float osc = 0.88f * s + 0.18f * tri;

        v.phase += v.phase_inc;
        const float two_pi = 2.0f * (float)std::numbers::pi;
        if (v.phase > two_pi) v.phase -= two_pi;

        if (v.samples_until_release > 0) v.samples_until_release--;
        if (v.samples_until_release == 0) v.env.note_off((float)sr);

        float env = v.env.tick((float)sr);
        if (v.env.stage == dsp::Adsr::Off) {
          v.active = false;
          continue;
        }

        float sample = osc * env * v.velocity;
        sample = v.filter.process(sample, g, k);

        // Simple stereo spread by MIDI note.
        float pan = 0.5f + 0.25f * std::sin((float)v.midi * 0.37f);
        l += sample * (1.0f - pan);
        r += sample * pan;
      }

      // FX (send/return style).
      float dl = l, dr = r;
      delay.process(dl, dr);

      float rv_l = l, rv_r = r;
      reverb.process(rv_l, rv_r);

      const float dm = std::clamp(delay_mix01.load(std::memory_order_relaxed), 0.0f, 1.0f);
      const float rm = std::clamp(reverb_mix01.load(std::memory_order_relaxed), 0.0f, 1.0f);
      const float wet = std::clamp(dm + rm, 0.0f, 1.0f);
      const float dry_gain = 1.0f - wet * 0.85f;

      float o_l = l * dry_gain + dl * dm + rv_l * rm;
      float o_r = r * dry_gain + dr * dm + rv_r * rm;

      const float mg = std::clamp(master_gain.load(std::memory_order_relaxed), 0.0f, 2.0f);
      o_l *= mg;
      o_r *= mg;

      // Limiter (very simple, per-sample).
      const float peak = std::max(std::abs(o_l), std::abs(o_r));
      const float thr = 0.95f;
      if (peak * limiter_gain > thr && peak > 1e-6f) {
        float target = thr / peak;
        limiter_gain = std::min(limiter_gain, target);
      } else {
        limiter_gain += (1.0f - limiter_gain) * 0.0008f; // release
        if (limiter_gain > 1.0f) limiter_gain = 1.0f;
      }

      o_l *= limiter_gain;
      o_r *= limiter_gain;

      // Final soft saturation.
      auto sat = [](float x) -> float { return x / (1.0f + std::abs(x)); };
      out[i * 2 + 0] = sat(o_l);
      out[i * 2 + 1] = sat(o_r);
    }
  }

  static bool pick_device_id(const AudioConfig& cfg, ma_context* ctx, ma_device_id* out_id, std::string* out_name) {
    if (!ctx || !out_id) return false;

    ma_device_info* play = nullptr;
    ma_uint32 play_count = 0;
    ma_device_info* cap = nullptr;
    ma_uint32 cap_count = 0;
    if (ma_context_get_devices(ctx, &play, &play_count, &cap, &cap_count) != MA_SUCCESS) return false;
    (void)cap; (void)cap_count;

    auto match_id = [&](const ma_device_info& di, const std::string& want_hex) -> bool {
      const std::string hex = hex_encode(&di.id, sizeof(di.id));
      return hex == want_hex;
    };

    const std::string want = cfg.device;
    if (!want.empty() && want.rfind("id:", 0) == 0) {
      const std::string hex = want.substr(3);
      for (ma_uint32 i = 0; i < play_count; i++) {
        if (match_id(play[i], hex)) {
          *out_id = play[i].id;
          if (out_name) *out_name = play[i].name;
          return true;
        }
      }
      return false;
    }

    if (!want.empty()) {
      for (ma_uint32 i = 0; i < play_count; i++) {
        if (icontains(play[i].name, want)) {
          *out_id = play[i].id;
          if (out_name) *out_name = play[i].name;
          return true;
        }
      }
    }

    for (ma_uint32 i = 0; i < play_count; i++) {
      if (play[i].isDefault) {
        *out_id = play[i].id;
        if (out_name) *out_name = play[i].name;
        return true;
      }
    }
    if (play_count > 0) {
      *out_id = play[0].id;
      if (out_name) *out_name = play[0].name;
      return true;
    }
    return false;
  }

  bool start_device(std::string* err) {
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = (ma_uint32)cfg.sample_rate;
    dc.dataCallback = &Impl::data_cb;
    dc.pUserData = this;

    std::string picked_name;
    has_chosen_playback_id = false;
    if (pick_device_id(cfg, &ctx, &chosen_playback_id, &picked_name)) {
      has_chosen_playback_id = true;
      dc.playback.pDeviceID = &chosen_playback_id;
      device_name = picked_name;
    } else {
      device_name = "default";
    }

    if (ma_device_init(&ctx, &dc, &device) != MA_SUCCESS) {
      if (err) *err = "ma_device_init failed (audio device unavailable?)";
      return false;
    }
    device_inited.store(true, std::memory_order_release);

    delay.init((uint32_t)cfg.sample_rate, 0.26f, 0.28f);
    reverb.init((uint32_t)cfg.sample_rate);

    if (ma_device_start(&device) != MA_SUCCESS) {
      if (err) *err = "ma_device_start failed";
      return false;
    }

    backend_name = device.pContext ? ma_get_backend_name(device.pContext->backend) : "unknown";
    std::fprintf(stderr, "khor-audio: backend=%s device=%s sr=%d\n",
      backend_name.c_str(), device_name.c_str(), cfg.sample_rate);
    return true;
  }

  void stop_device() {
    if (device_inited.load(std::memory_order_acquire)) ma_device_uninit(&device);
    device_inited.store(false, std::memory_order_release);
    backend_name.clear();
    device_name.clear();
  }
};

AudioEngine::AudioEngine() : impl_(new Impl()) {}
AudioEngine::~AudioEngine() { stop(); delete impl_; impl_ = nullptr; }

bool AudioEngine::start(const AudioConfig& cfg, std::string* err) {
  if (!impl_) return false;
  if (impl_->device_inited.load(std::memory_order_acquire)) return true;

  impl_->cfg = cfg;
  impl_->master_gain.store(cfg.master_gain, std::memory_order_relaxed);

  impl_->backend_count = 0;
  ma_backend forced{};
  std::string backend = cfg.backend;
  if (const char* env = std::getenv("KHOR_AUDIO_BACKEND"); env && *env) backend = env;
  if (parse_backend(backend, &forced)) {
    impl_->backends[impl_->backend_count++] = forced;
  } else {
    impl_->backends[impl_->backend_count++] = ma_backend_pulseaudio;
    impl_->backends[impl_->backend_count++] = ma_backend_alsa;
  }
  if (std::getenv("KHOR_AUDIO_ALLOW_NULL") != nullptr) {
    impl_->backends[impl_->backend_count++] = ma_backend_null;
  }

  if (!impl_->init_context(err)) return false;
  if (!impl_->start_device(err)) {
    impl_->stop_device();
    impl_->uninit_context();
    return false;
  }
  return true;
}

void AudioEngine::stop() {
  if (!impl_) return;
  impl_->stop_device();
  impl_->uninit_context();
}

bool AudioEngine::restart(const AudioConfig& cfg, std::string* err) {
  stop();
  return start(cfg, err);
}

bool AudioEngine::is_running() const { return impl_ && impl_->device_inited.load(std::memory_order_acquire); }

std::string AudioEngine::backend_name() const { return impl_ ? impl_->backend_name : ""; }
std::string AudioEngine::device_name() const { return impl_ ? impl_->device_name : ""; }

void AudioEngine::submit_note(const NoteEvent& ev) {
  if (!impl_ || !impl_->device_inited.load(std::memory_order_acquire)) return;
  if (!impl_->q.push(ev)) {
    impl_->q_drops.fetch_add(1, std::memory_order_relaxed);
  }
}

void AudioEngine::set_master_gain(float gain) {
  if (!impl_) return;
  impl_->master_gain.store(gain, std::memory_order_relaxed);
}

void AudioEngine::set_filter(float cutoff01, float resonance01) {
  if (!impl_) return;
  impl_->cutoff01.store(cutoff01, std::memory_order_relaxed);
  impl_->resonance01.store(resonance01, std::memory_order_relaxed);
}

void AudioEngine::set_fx(float delay_mix01, float reverb_mix01) {
  if (!impl_) return;
  impl_->delay_mix01.store(delay_mix01, std::memory_order_relaxed);
  impl_->reverb_mix01.store(reverb_mix01, std::memory_order_relaxed);
}

bool AudioEngine::enumerate_playback_devices(const AudioConfig& cfg, std::vector<AudioDeviceInfo>* out, std::string* err) {
  if (!out) return false;
  out->clear();

  ma_backend backends[3]{};
  ma_uint32 backend_count = 0;
  ma_backend forced{};
  if (parse_backend(cfg.backend, &forced)) {
    backends[backend_count++] = forced;
  } else {
    backends[backend_count++] = ma_backend_pulseaudio;
    backends[backend_count++] = ma_backend_alsa;
  }

  ma_context ctx{};
  if (ma_context_init(backends, backend_count, nullptr, &ctx) != MA_SUCCESS) {
    if (err) *err = "ma_context_init failed";
    return false;
  }

  ma_device_info* play = nullptr;
  ma_uint32 play_count = 0;
  ma_device_info* cap = nullptr;
  ma_uint32 cap_count = 0;
  ma_result r = ma_context_get_devices(&ctx, &play, &play_count, &cap, &cap_count);
  (void)cap;
  (void)cap_count;
  if (r != MA_SUCCESS) {
    ma_context_uninit(&ctx);
    if (err) *err = "ma_context_get_devices failed";
    return false;
  }

  out->reserve(play_count);
  for (ma_uint32 i = 0; i < play_count; i++) {
    AudioDeviceInfo di;
    di.id = hex_encode(&play[i].id, sizeof(play[i].id));
    di.name = play[i].name;
    di.is_default = play[i].isDefault == MA_TRUE;
    out->push_back(std::move(di));
  }

  ma_context_uninit(&ctx);
  return true;
}

} // namespace khor
