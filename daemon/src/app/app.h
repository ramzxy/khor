#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/config.h"
#include "audio/engine.h"
#include "bpf/collector.h"
#include "engine/music.h"
#include "engine/signals.h"
#include "khor/metrics.h"
#include "midi/alsa_seq.h"
#include "osc/osc.h"
#include "util/json.h"

namespace khor {

class App {
 public:
  App(std::string config_path, KhorConfig cfg);
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  bool start(std::string* err);
  void stop();
  bool is_running() const { return running_.load(); }

  std::string config_path() const { return config_path_; }

  KhorConfig config_snapshot() const;

  JsonValue api_health() const;
  JsonValue api_metrics(bool include_history) const;
  JsonValue api_presets() const;

  // Applies a JSON config patch (same schema as /api/config) and persists the result.
  // Returns the updated full config JSON with {"ok":true,"restart_required":...}.
  bool api_put_config(const JsonValue& patch, JsonValue* out, int* http_status);

  bool api_select_preset(const std::string& name, std::string* err);
  bool api_test_note(int midi, float vel, double dur_s, std::string* err);

  bool api_audio_devices(std::vector<AudioDeviceInfo>* out, std::string* err) const;
  bool api_audio_set_device(const std::string& device, std::string* err);

 private:
  struct HistSample {
    int64_t ts_ms = 0;
    SignalRates rates{};
  };

  void sampler_loop();
  void music_loop();
  void fake_loop();

  bool start_audio_locked(const KhorConfig& cfg, std::string* err);
  void stop_audio_locked();
  bool restart_audio_locked(const KhorConfig& cfg, std::string* err);

  bool start_midi_locked(const KhorConfig& cfg, std::string* err);
  void stop_midi_locked();

  bool start_osc_locked(const KhorConfig& cfg, std::string* err);
  void stop_osc_locked();

  bool start_bpf_locked(const KhorConfig& cfg, std::string* err);
  void stop_bpf_locked();
  void apply_bpf_cfg_locked(const KhorConfig& cfg);

  static int64_t unix_ms_now();

  std::string config_path_;

  mutable std::mutex cfg_mu_;
  KhorConfig cfg_;

  // Hot controls (avoid holding cfg_mu_ in loops).
  std::atomic<double> density_{0.35};
  std::atomic<double> smoothing_{0.85};

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};

  // Modules.
  KhorMetrics metrics_{};

  AudioEngine audio_{};
  mutable std::mutex audio_mu_;
  std::string audio_err_;

  MidiOut midi_{};
  mutable std::mutex midi_mu_;
  std::string midi_err_;

  OscClient osc_{};
  mutable std::mutex osc_mu_;
  std::string osc_err_;

  BpfCollector bpf_{};
  mutable std::mutex bpf_mu_;
  std::string bpf_err_;

  std::atomic<bool> fake_running_{false};

  // Signals + history.
  mutable std::mutex sig_mu_;
  Signals signals_{};
  SignalRates last_rates_{};
  Signal01 last_v01_{};

  mutable std::mutex hist_mu_;
  std::deque<HistSample> history_;

  // Threads.
  std::thread sampler_;
  std::thread music_;
  std::thread fake_;
};

} // namespace khor

