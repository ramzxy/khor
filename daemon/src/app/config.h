#pragma once

#include <cstdint>
#include <string>

#include "util/json.h"

namespace khor {

struct KhorConfig {
  int version = 1;

  std::string listen_host = "127.0.0.1";
  int listen_port = 17321;

  bool serve_ui = true;
  std::string ui_dir; // empty => use default

  bool enable_bpf = true;
  bool enable_audio = true;
  bool enable_midi = false;
  bool enable_osc = false;
  bool enable_fake = false;

  // eBPF
  uint32_t bpf_enabled_mask = 0xFFFFFFFFu;
  uint32_t bpf_sample_interval_ms = 200;
  uint32_t bpf_tgid_allow = 0;
  uint32_t bpf_tgid_deny = 0;
  uint64_t bpf_cgroup_id = 0;

  // Music
  double bpm = 110.0;
  int key_midi = 62; // D4
  std::string scale = "pentatonic_minor";
  std::string preset = "ambient";
  double density = 0.35;   // 0..1
  double smoothing = 0.85; // 0..1

  // Audio
  std::string audio_backend; // "" | "pulseaudio" | "alsa" | "null"
  std::string audio_device;  // substring match
  int audio_sample_rate = 48000;
  float audio_master_gain = 0.25f;

  // MIDI
  std::string midi_port = "khor";
  int midi_channel = 1; // 1..16

  // OSC
  std::string osc_host = "127.0.0.1";
  int osc_port = 9000;
};

JsonValue config_to_json(const KhorConfig& cfg);
bool config_from_json(const JsonValue& root, KhorConfig* cfg, std::string* err);

bool load_config_file(const std::string& path, KhorConfig* cfg, std::string* err);
bool save_config_file(const std::string& path, const KhorConfig& cfg, std::string* err);

} // namespace khor
