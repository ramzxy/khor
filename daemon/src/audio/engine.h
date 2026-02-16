#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/note_event.h"

namespace khor {

struct AudioDeviceInfo {
  std::string id;   // hex string (backend-specific bytes)
  std::string name; // display name
  bool is_default = false;
};

struct AudioConfig {
  std::string backend;     // "" | "pulseaudio" | "alsa" | "null"
  std::string device;      // "" (default) | substring match | "id:<hex>"
  int sample_rate = 48000; // Hz
  float master_gain = 0.25f;
};

struct AudioStatus {
  bool enabled = false;
  bool ok = false;
  std::string backend;
  std::string device;
  std::string error;
};

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  bool start(const AudioConfig& cfg, std::string* err);
  void stop();
  bool restart(const AudioConfig& cfg, std::string* err);

  bool is_running() const;

  std::string backend_name() const;
  std::string device_name() const;

  void submit_note(const NoteEvent& ev);

  // Real-time safe (atomic).
  void set_master_gain(float gain);
  void set_filter(float cutoff01, float resonance01);
  void set_fx(float delay_mix01, float reverb_mix01);

  static bool enumerate_playback_devices(const AudioConfig& cfg, std::vector<AudioDeviceInfo>* out, std::string* err);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace khor

