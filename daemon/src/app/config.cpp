#include "app/config.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/paths.h"

namespace khor {

static const JsonValue* obj_get_obj(const JsonValue& obj, const char* key) {
  const JsonValue* v = json_get(obj, key);
  if (!v || !v->is_object()) return nullptr;
  return v;
}

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static double clamp_double(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

JsonValue config_to_json(const KhorConfig& cfg) {
  JsonValue root = JsonValue::make_object({});

  root.o["version"] = JsonValue::make_number(cfg.version);

  root.o["listen"] = JsonValue::make_object({
    {"host", JsonValue::make_string(cfg.listen_host)},
    {"port", JsonValue::make_number(cfg.listen_port)},
  });

  root.o["ui"] = JsonValue::make_object({
    {"serve", JsonValue::make_bool(cfg.serve_ui)},
    {"dir", JsonValue::make_string(cfg.ui_dir)},
  });

  root.o["features"] = JsonValue::make_object({
    {"bpf", JsonValue::make_bool(cfg.enable_bpf)},
    {"audio", JsonValue::make_bool(cfg.enable_audio)},
    {"midi", JsonValue::make_bool(cfg.enable_midi)},
    {"osc", JsonValue::make_bool(cfg.enable_osc)},
    {"fake", JsonValue::make_bool(cfg.enable_fake)},
  });

  root.o["bpf"] = JsonValue::make_object({
    {"enabled_mask", JsonValue::make_number((double)cfg.bpf_enabled_mask)},
    {"sample_interval_ms", JsonValue::make_number((double)cfg.bpf_sample_interval_ms)},
  });

  root.o["music"] = JsonValue::make_object({
    {"bpm", JsonValue::make_number(cfg.bpm)},
    {"key_midi", JsonValue::make_number(cfg.key_midi)},
    {"scale", JsonValue::make_string(cfg.scale)},
    {"preset", JsonValue::make_string(cfg.preset)},
    {"density", JsonValue::make_number(cfg.density)},
    {"smoothing", JsonValue::make_number(cfg.smoothing)},
  });

  root.o["audio"] = JsonValue::make_object({
    {"backend", JsonValue::make_string(cfg.audio_backend)},
    {"device", JsonValue::make_string(cfg.audio_device)},
    {"sample_rate", JsonValue::make_number(cfg.audio_sample_rate)},
    {"master_gain", JsonValue::make_number(cfg.audio_master_gain)},
  });

  root.o["midi"] = JsonValue::make_object({
    {"port", JsonValue::make_string(cfg.midi_port)},
    {"channel", JsonValue::make_number(cfg.midi_channel)},
  });

  root.o["osc"] = JsonValue::make_object({
    {"host", JsonValue::make_string(cfg.osc_host)},
    {"port", JsonValue::make_number(cfg.osc_port)},
  });

  return root;
}

bool config_from_json(const JsonValue& root, KhorConfig* cfg, std::string* err) {
  if (!cfg) return false;
  if (!root.is_object()) {
    if (err) *err = "config root must be a JSON object";
    return false;
  }

  // version
  cfg->version = (int)json_get_number(root, "version", cfg->version);

  // listen
  if (const JsonValue* listen = obj_get_obj(root, "listen")) {
    cfg->listen_host = json_get_string(*listen, "host", cfg->listen_host);
    cfg->listen_port = clamp_int((int)json_get_number(*listen, "port", cfg->listen_port), 1, 65535);
  }

  // ui
  if (const JsonValue* ui = obj_get_obj(root, "ui")) {
    cfg->serve_ui = json_get_bool(*ui, "serve", cfg->serve_ui);
    cfg->ui_dir = json_get_string(*ui, "dir", cfg->ui_dir);
  }

  // features
  if (const JsonValue* f = obj_get_obj(root, "features")) {
    cfg->enable_bpf = json_get_bool(*f, "bpf", cfg->enable_bpf);
    cfg->enable_audio = json_get_bool(*f, "audio", cfg->enable_audio);
    cfg->enable_midi = json_get_bool(*f, "midi", cfg->enable_midi);
    cfg->enable_osc = json_get_bool(*f, "osc", cfg->enable_osc);
    cfg->enable_fake = json_get_bool(*f, "fake", cfg->enable_fake);
  }

  // bpf
  if (const JsonValue* bpf = obj_get_obj(root, "bpf")) {
    cfg->bpf_enabled_mask = (uint32_t)json_get_number(*bpf, "enabled_mask", cfg->bpf_enabled_mask);
    cfg->bpf_sample_interval_ms = (uint32_t)json_get_number(*bpf, "sample_interval_ms", cfg->bpf_sample_interval_ms);
    cfg->bpf_sample_interval_ms = std::clamp(cfg->bpf_sample_interval_ms, 10u, 5000u);
  }

  // music
  if (const JsonValue* m = obj_get_obj(root, "music")) {
    cfg->bpm = clamp_double(json_get_number(*m, "bpm", cfg->bpm), 1.0, 400.0);
    cfg->key_midi = clamp_int((int)json_get_number(*m, "key_midi", cfg->key_midi), 0, 127);
    cfg->scale = json_get_string(*m, "scale", cfg->scale);
    cfg->preset = json_get_string(*m, "preset", cfg->preset);
    cfg->density = clamp_double(json_get_number(*m, "density", cfg->density), 0.0, 1.0);
    cfg->smoothing = clamp_double(json_get_number(*m, "smoothing", cfg->smoothing), 0.0, 1.0);
  }

  // audio
  if (const JsonValue* a = obj_get_obj(root, "audio")) {
    cfg->audio_backend = json_get_string(*a, "backend", cfg->audio_backend);
    cfg->audio_device = json_get_string(*a, "device", cfg->audio_device);
    cfg->audio_sample_rate = clamp_int((int)json_get_number(*a, "sample_rate", cfg->audio_sample_rate), 8000, 192000);
    cfg->audio_master_gain = (float)clamp_double(json_get_number(*a, "master_gain", cfg->audio_master_gain), 0.0, 2.0);
  }

  // midi
  if (const JsonValue* m = obj_get_obj(root, "midi")) {
    cfg->midi_port = json_get_string(*m, "port", cfg->midi_port);
    cfg->midi_channel = clamp_int((int)json_get_number(*m, "channel", cfg->midi_channel), 1, 16);
  }

  // osc
  if (const JsonValue* o = obj_get_obj(root, "osc")) {
    cfg->osc_host = json_get_string(*o, "host", cfg->osc_host);
    cfg->osc_port = clamp_int((int)json_get_number(*o, "port", cfg->osc_port), 1, 65535);
  }

  // Back-compat for very old flat keys (best-effort).
  cfg->bpm = clamp_double(json_get_number(root, "bpm", cfg->bpm), 1.0, 400.0);
  cfg->key_midi = clamp_int((int)json_get_number(root, "key_midi", cfg->key_midi), 0, 127);

  return true;
}

bool load_config_file(const std::string& path, KhorConfig* cfg, std::string* err) {
  if (!cfg) return false;

  std::ifstream f(path);
  if (!f.good()) {
    // Missing config is not an error.
    return true;
  }

  std::ostringstream ss;
  ss << f.rdbuf();
  std::string content = ss.str();

  JsonValue root;
  JsonParseError perr;
  if (!json_parse(content, &root, &perr)) {
    if (err) {
      *err = "failed to parse config JSON: " + perr.message;
    }
    return false;
  }

  return config_from_json(root, cfg, err);
}

bool save_config_file(const std::string& path, const KhorConfig& cfg, std::string* err) {
  try {
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path());

    JsonValue root = config_to_json(cfg);
    std::string out = json_stringify(root, 2);

    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) {
      if (err) {
        *err = "failed to open config for write: " + path + ": " + std::strerror(errno);
      }
      return false;
    }
    f << out;
    f.flush();
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

} // namespace khor
