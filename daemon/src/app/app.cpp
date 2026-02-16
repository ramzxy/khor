#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "util/paths.h"

namespace khor {
namespace {

static JsonValue json_ok(bool ok) {
  return JsonValue::make_object({{"ok", JsonValue::make_bool(ok)}});
}

static JsonValue json_error(const std::string& msg) {
  return JsonValue::make_object({
    {"ok", JsonValue::make_bool(false)},
    {"error", JsonValue::make_string(msg)},
  });
}

} // namespace

int64_t App::unix_ms_now() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
}

App::App(std::string config_path, KhorConfig cfg)
  : config_path_(std::move(config_path)), cfg_(std::move(cfg)) {
  if (cfg_.ui_dir.empty()) cfg_.ui_dir = path_default_ui_dir();
  density_.store(cfg_.density);
  smoothing_.store(cfg_.smoothing);
  metrics_.bpm.store(cfg_.bpm);
  metrics_.key_midi.store(cfg_.key_midi);
}

App::~App() { stop(); }

KhorConfig App::config_snapshot() const {
  std::scoped_lock lk(cfg_mu_);
  return cfg_;
}

bool App::start_audio_locked(const KhorConfig& cfg, std::string* err) {
  AudioConfig ac;
  ac.backend = cfg.audio_backend;
  ac.device = cfg.audio_device;
  ac.sample_rate = cfg.audio_sample_rate;
  ac.master_gain = cfg.audio_master_gain;

  std::string e;
  bool ok = audio_.start(ac, &e);
  audio_.set_master_gain(cfg.audio_master_gain);
  if (!ok) {
    audio_err_ = e.empty() ? "audio init failed" : e;
    if (err) *err = audio_err_;
    return false;
  }
  audio_err_.clear();
  return true;
}

void App::stop_audio_locked() {
  audio_.stop();
}

bool App::restart_audio_locked(const KhorConfig& cfg, std::string* err) {
  AudioConfig ac;
  ac.backend = cfg.audio_backend;
  ac.device = cfg.audio_device;
  ac.sample_rate = cfg.audio_sample_rate;
  ac.master_gain = cfg.audio_master_gain;

  std::string e;
  bool ok = audio_.restart(ac, &e);
  audio_.set_master_gain(cfg.audio_master_gain);
  if (!ok) {
    audio_err_ = e.empty() ? "audio init failed" : e;
    if (err) *err = audio_err_;
    return false;
  }
  audio_err_.clear();
  return true;
}

bool App::start_midi_locked(const KhorConfig& cfg, std::string* err) {
  std::string e;
  bool ok = midi_.start(cfg.midi_port, cfg.midi_channel, &e);
  if (!ok) {
    midi_err_ = e.empty() ? "midi init failed" : e;
    if (err) *err = midi_err_;
    return false;
  }
  midi_err_.clear();
  return true;
}

void App::stop_midi_locked() { midi_.stop(); }

bool App::start_osc_locked(const KhorConfig& cfg, std::string* err) {
  std::string e;
  bool ok = osc_.start(cfg.osc_host, cfg.osc_port, &e);
  if (!ok) {
    osc_err_ = e.empty() ? "osc init failed" : e;
    if (err) *err = osc_err_;
    return false;
  }
  osc_err_.clear();
  return true;
}

void App::stop_osc_locked() { osc_.stop(); }

static BpfConfig make_bpf_cfg(const KhorConfig& cfg) {
  BpfConfig b;
  b.enabled = cfg.enable_bpf;
  b.enabled_mask = cfg.bpf_enabled_mask;
  b.sample_interval_ms = cfg.bpf_sample_interval_ms;
  b.tgid_allow = cfg.bpf_tgid_allow;
  b.tgid_deny = cfg.bpf_tgid_deny;
  b.cgroup_id = cfg.bpf_cgroup_id;
  return b;
}

bool App::start_bpf_locked(const KhorConfig& cfg, std::string* err) {
  std::string e;
  bool ok = bpf_.start(make_bpf_cfg(cfg), &metrics_, &e);
  if (!ok) {
    bpf_err_ = e.empty() ? "bpf init failed" : e;
    if (err) *err = bpf_err_;
    return false;
  }
  bpf_err_.clear();
  return true;
}

void App::stop_bpf_locked() { bpf_.stop(); }

void App::apply_bpf_cfg_locked(const KhorConfig& cfg) {
  (void)bpf_.apply_config(make_bpf_cfg(cfg), nullptr);
}

bool App::start(std::string* err) {
  if (running_.load()) return true;
  stop_.store(false);
  running_.store(true);

  KhorConfig cfg = config_snapshot();
  metrics_.bpm.store(cfg.bpm);
  metrics_.key_midi.store(cfg.key_midi);
  density_.store(cfg.density);
  smoothing_.store(cfg.smoothing);

  // Start outputs + BPF. Failures are reported via /api/health but don't stop the daemon.
  if (cfg.enable_audio) {
    std::scoped_lock lk(audio_mu_);
    (void)start_audio_locked(cfg, err);
  }
  if (cfg.enable_midi) {
    std::scoped_lock lk(midi_mu_);
    (void)start_midi_locked(cfg, err);
  }
  if (cfg.enable_osc) {
    std::scoped_lock lk(osc_mu_);
    (void)start_osc_locked(cfg, err);
  }

  if (cfg.enable_bpf) {
    std::scoped_lock lk(bpf_mu_);
    (void)start_bpf_locked(cfg, err);
  } else {
    std::scoped_lock lk(bpf_mu_);
    bpf_err_ = "disabled by config";
  }

  // Fake metrics mode only if explicitly enabled and BPF isn't ok.
  if (cfg.enable_fake && !bpf_.status().ok) {
    fake_running_.store(true);
    fake_ = std::thread([this] { fake_loop(); });
  }

  sampler_ = std::thread([this] { sampler_loop(); });
  music_ = std::thread([this] { music_loop(); });

  return true;
}

void App::stop() {
  if (!running_.exchange(false)) return;
  stop_.store(true);

  fake_running_.store(false);

  if (music_.joinable()) music_.join();
  if (sampler_.joinable()) sampler_.join();
  if (fake_.joinable()) fake_.join();

  {
    std::scoped_lock lk(bpf_mu_);
    stop_bpf_locked();
  }
  {
    std::scoped_lock lk(osc_mu_);
    stop_osc_locked();
  }
  {
    std::scoped_lock lk(midi_mu_);
    stop_midi_locked();
  }
  {
    std::scoped_lock lk(audio_mu_);
    stop_audio_locked();
  }
}

void App::sampler_loop() {
  using clock = std::chrono::steady_clock;
  auto last_t = clock::now();

  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto now = clock::now();
    double dt_s = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_t).count();
    if (dt_s <= 0.0) dt_s = 0.1;
    last_t = now;

    Signals::Totals t;
    t.exec_total = metrics_.exec_total.load(std::memory_order_relaxed);
    t.net_rx_bytes_total = metrics_.net_rx_bytes_total.load(std::memory_order_relaxed);
    t.net_tx_bytes_total = metrics_.net_tx_bytes_total.load(std::memory_order_relaxed);
    t.sched_switch_total = metrics_.sched_switch_total.load(std::memory_order_relaxed);
    t.blk_read_bytes_total = metrics_.blk_read_bytes_total.load(std::memory_order_relaxed);
    t.blk_write_bytes_total = metrics_.blk_write_bytes_total.load(std::memory_order_relaxed);

    const double smoothing = std::clamp(smoothing_.load(std::memory_order_relaxed), 0.0, 1.0);

    {
      std::scoped_lock lk(sig_mu_);
      signals_.update(t, dt_s, smoothing);
      last_rates_ = signals_.rates();
      last_v01_ = signals_.value01();
    }

    {
      std::scoped_lock lk(hist_mu_);
      history_.push_back(HistSample{
        .ts_ms = unix_ms_now(),
        .rates = last_rates_,
      });
      while (history_.size() > 600) history_.pop_front();
    }
  }
}

void App::music_loop() {
  MusicEngine engine;
  uint32_t osc_signal_tick = 0;
  uint32_t osc_metrics_tick = 0;

  using clock = std::chrono::steady_clock;
  auto next = clock::now();

  while (!stop_.load()) {
    const double bpm = metrics_.bpm.load(std::memory_order_relaxed);
    const double ms = MusicEngine::tick_ms(bpm);
    next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(ms));
    std::this_thread::sleep_until(next);
    if (stop_.load()) break;

    KhorConfig cfg = config_snapshot();

    Signal01 s01;
    SignalRates rates;
    {
      std::scoped_lock lk(sig_mu_);
      s01 = last_v01_;
      rates = last_rates_;
    }

    MusicConfig mc;
    mc.bpm = bpm;
    mc.key_midi = metrics_.key_midi.load(std::memory_order_relaxed);
    mc.scale = cfg.scale;
    mc.preset = cfg.preset;
    mc.density = std::clamp(density_.load(std::memory_order_relaxed), 0.0, 1.0);

    MusicFrame frame = engine.tick(s01, mc);

    // Apply synth params.
    if (cfg.enable_audio && audio_.is_running()) {
      audio_.set_filter(frame.synth.cutoff01, frame.synth.resonance01);
      audio_.set_fx(frame.synth.delay_mix01, frame.synth.reverb_mix01);
    }

    // Emit notes.
    for (const auto& n : frame.notes) {
      if (cfg.enable_audio && audio_.is_running()) audio_.submit_note(n);
      if (cfg.enable_midi && midi_.is_running()) midi_.send_note(n);
      if (cfg.enable_osc && osc_.is_running()) osc_.send_note(n);
    }

    if (cfg.enable_midi && midi_.is_running()) {
      midi_.send_signals_cc(s01, frame.synth.cutoff01);
    }

    if (cfg.enable_osc && osc_.is_running()) {
      // Throttle OSC signal spam.
      if ((osc_signal_tick++ & 3u) == 0u) {
        osc_.send_signal("exec", (float)s01.exec);
        osc_.send_signal("rx", (float)s01.rx);
        osc_.send_signal("tx", (float)s01.tx);
        osc_.send_signal("csw", (float)s01.csw);
        osc_.send_signal("io", (float)s01.io);
      }
      if ((osc_metrics_tick++ & 7u) == 0u) {
        osc_.send_metrics(rates);
      }
    }
  }
}

void App::fake_loop() {
  while (!stop_.load() && fake_running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    metrics_.exec_total.fetch_add(1, std::memory_order_relaxed);
    metrics_.net_rx_bytes_total.fetch_add(1000 + (std::rand() % 60000), std::memory_order_relaxed);
    metrics_.net_tx_bytes_total.fetch_add(1000 + (std::rand() % 40000), std::memory_order_relaxed);
    metrics_.sched_switch_total.fetch_add(5 + (std::rand() % 200), std::memory_order_relaxed);
    metrics_.blk_read_bytes_total.fetch_add(4096 * (std::rand() % 8), std::memory_order_relaxed);
    metrics_.blk_write_bytes_total.fetch_add(4096 * (std::rand() % 6), std::memory_order_relaxed);
  }
}

JsonValue App::api_health() const {
  KhorConfig cfg = config_snapshot();

  JsonValue root = JsonValue::make_object({});
  root.o["ts_ms"] = JsonValue::make_number((double)unix_ms_now());
  root.o["config_path"] = JsonValue::make_string(config_path_);

  {
    JsonValue a = JsonValue::make_object({});
    a.o["enabled"] = JsonValue::make_bool(cfg.enable_audio);
    a.o["ok"] = JsonValue::make_bool(audio_.is_running());
    a.o["backend"] = JsonValue::make_string(audio_.backend_name().empty() ? "none" : audio_.backend_name());
    a.o["device"] = JsonValue::make_string(audio_.device_name().empty() ? "none" : audio_.device_name());
    std::scoped_lock lk(audio_mu_);
    if (!audio_err_.empty()) a.o["error"] = JsonValue::make_string(audio_err_);
    root.o["audio"] = std::move(a);
  }

  {
    JsonValue m = JsonValue::make_object({});
    m.o["enabled"] = JsonValue::make_bool(cfg.enable_midi);
    m.o["ok"] = JsonValue::make_bool(midi_.is_running());
    m.o["port"] = JsonValue::make_string(cfg.midi_port);
    m.o["channel"] = JsonValue::make_number(cfg.midi_channel);
    std::scoped_lock lk(midi_mu_);
    if (!midi_err_.empty()) m.o["error"] = JsonValue::make_string(midi_err_);
    root.o["midi"] = std::move(m);
  }

  {
    JsonValue o = JsonValue::make_object({});
    o.o["enabled"] = JsonValue::make_bool(cfg.enable_osc);
    o.o["ok"] = JsonValue::make_bool(osc_.is_running());
    o.o["host"] = JsonValue::make_string(cfg.osc_host);
    o.o["port"] = JsonValue::make_number(cfg.osc_port);
    std::scoped_lock lk(osc_mu_);
    if (!osc_err_.empty()) o.o["error"] = JsonValue::make_string(osc_err_);
    root.o["osc"] = std::move(o);
  }

  {
    JsonValue b = JsonValue::make_object({});
    b.o["enabled"] = JsonValue::make_bool(cfg.enable_bpf);
    const BpfStatus st = bpf_.status();
    b.o["ok"] = JsonValue::make_bool(st.ok);
    b.o["err_code"] = JsonValue::make_number((double)st.err_code);
    {
      std::scoped_lock lk(bpf_mu_);
      const std::string e = !bpf_err_.empty() ? bpf_err_ : st.error;
      if (!e.empty()) b.o["error"] = JsonValue::make_string(e);
    }
    root.o["bpf"] = std::move(b);
  }

  root.o["features"] = JsonValue::make_object({
    {"fake", JsonValue::make_bool(cfg.enable_fake)},
  });

  return root;
}

JsonValue App::api_metrics(bool include_history) const {
  JsonValue root = JsonValue::make_object({});
  root.o["ts_ms"] = JsonValue::make_number((double)unix_ms_now());

  root.o["totals"] = JsonValue::make_object({
    {"events_total", JsonValue::make_number((double)metrics_.events_total.load(std::memory_order_relaxed))},
    {"events_dropped", JsonValue::make_number((double)metrics_.events_dropped.load(std::memory_order_relaxed))},
    {"exec_total", JsonValue::make_number((double)metrics_.exec_total.load(std::memory_order_relaxed))},
    {"net_rx_bytes_total", JsonValue::make_number((double)metrics_.net_rx_bytes_total.load(std::memory_order_relaxed))},
    {"net_tx_bytes_total", JsonValue::make_number((double)metrics_.net_tx_bytes_total.load(std::memory_order_relaxed))},
    {"sched_switch_total", JsonValue::make_number((double)metrics_.sched_switch_total.load(std::memory_order_relaxed))},
    {"blk_read_bytes_total", JsonValue::make_number((double)metrics_.blk_read_bytes_total.load(std::memory_order_relaxed))},
    {"blk_write_bytes_total", JsonValue::make_number((double)metrics_.blk_write_bytes_total.load(std::memory_order_relaxed))},
  });

  SignalRates r{};
  {
    std::scoped_lock lk(sig_mu_);
    r = last_rates_;
  }
  root.o["rates"] = JsonValue::make_object({
    {"exec_s", JsonValue::make_number(r.exec_s)},
    {"rx_kbs", JsonValue::make_number(r.rx_kbs)},
    {"tx_kbs", JsonValue::make_number(r.tx_kbs)},
    {"csw_s", JsonValue::make_number(r.csw_s)},
    {"blk_r_kbs", JsonValue::make_number(r.blk_r_kbs)},
    {"blk_w_kbs", JsonValue::make_number(r.blk_w_kbs)},
  });

  root.o["controls"] = JsonValue::make_object({
    {"bpm", JsonValue::make_number(metrics_.bpm.load(std::memory_order_relaxed))},
    {"key_midi", JsonValue::make_number(metrics_.key_midi.load(std::memory_order_relaxed))},
    {"density", JsonValue::make_number(density_.load(std::memory_order_relaxed))},
    {"smoothing", JsonValue::make_number(smoothing_.load(std::memory_order_relaxed))},
  });

  if (include_history) {
    std::vector<JsonValue> arr;
    {
      std::scoped_lock lk(hist_mu_);
      arr.reserve(history_.size());
      for (const auto& s : history_) {
        JsonValue o = JsonValue::make_object({});
        o.o["ts_ms"] = JsonValue::make_number((double)s.ts_ms);
        o.o["exec_s"] = JsonValue::make_number(s.rates.exec_s);
        o.o["rx_kbs"] = JsonValue::make_number(s.rates.rx_kbs);
        o.o["tx_kbs"] = JsonValue::make_number(s.rates.tx_kbs);
        o.o["csw_s"] = JsonValue::make_number(s.rates.csw_s);
        o.o["blk_r_kbs"] = JsonValue::make_number(s.rates.blk_r_kbs);
        o.o["blk_w_kbs"] = JsonValue::make_number(s.rates.blk_w_kbs);
        arr.push_back(std::move(o));
      }
    }
    root.o["history"] = JsonValue::make_array(std::move(arr));
  }

  return root;
}

JsonValue App::api_presets() const {
  std::vector<JsonValue> arr;
  arr.push_back(JsonValue::make_object({
    {"name", JsonValue::make_string("ambient")},
    {"hint", JsonValue::make_string("slow, sparse, more reverb")},
  }));
  arr.push_back(JsonValue::make_object({
    {"name", JsonValue::make_string("percussive")},
    {"hint", JsonValue::make_string("tight envelope, scheduler-driven rhythm")},
  }));
  arr.push_back(JsonValue::make_object({
    {"name", JsonValue::make_string("arp")},
    {"hint", JsonValue::make_string("network-driven arpeggio + exec stabs")},
  }));
  arr.push_back(JsonValue::make_object({
    {"name", JsonValue::make_string("drone")},
    {"hint", JsonValue::make_string("IO controls timbre; sustained tones")},
  }));

  return JsonValue::make_object({{"presets", JsonValue::make_array(std::move(arr))}});
}

bool App::api_select_preset(const std::string& name, std::string* err) {
  if (name != "ambient" && name != "percussive" && name != "arp" && name != "drone") {
    if (err) *err = "unknown preset";
    return false;
  }

  KhorConfig next = config_snapshot();
  next.preset = name;

  if (name == "ambient") {
    next.density = 0.20;
    next.smoothing = 0.92;
  } else if (name == "percussive") {
    next.density = 0.80;
    next.smoothing = 0.35;
  } else if (name == "arp") {
    next.density = 0.55;
    next.smoothing = 0.60;
  } else if (name == "drone") {
    next.density = 0.10;
    next.smoothing = 0.95;
  }

  // Save + apply.
  {
    std::scoped_lock lk(cfg_mu_);
    cfg_ = next;
  }
  density_.store(next.density);
  smoothing_.store(next.smoothing);

  (void)save_config_file(config_path_, next, nullptr);
  return true;
}

bool App::api_test_note(int midi, float vel, double dur_s, std::string* err) {
  midi = std::clamp(midi, 0, 127);
  vel = std::clamp(vel, 0.0f, 1.0f);
  dur_s = std::clamp(dur_s, 0.02, 3.0);

  NoteEvent ev;
  ev.midi = midi;
  ev.velocity = vel;
  ev.dur_s = (float)dur_s;

  KhorConfig cfg = config_snapshot();
  bool any = false;

  if (cfg.enable_audio && audio_.is_running()) {
    audio_.submit_note(ev);
    any = true;
  }
  if (cfg.enable_midi && midi_.is_running()) {
    midi_.send_note(ev);
    any = true;
  }
  if (cfg.enable_osc && osc_.is_running()) {
    osc_.send_note(ev);
    any = true;
  }

  if (!any) {
    if (err) *err = "no outputs enabled/available for test_note";
    return false;
  }
  return true;
}

bool App::api_audio_devices(std::vector<AudioDeviceInfo>* out, std::string* err) const {
  if (!out) return false;
  KhorConfig cfg = config_snapshot();
  AudioConfig ac;
  ac.backend = cfg.audio_backend;
  ac.device = cfg.audio_device;
  ac.sample_rate = cfg.audio_sample_rate;
  ac.master_gain = cfg.audio_master_gain;
  return AudioEngine::enumerate_playback_devices(ac, out, err);
}

bool App::api_audio_set_device(const std::string& device, std::string* err) {
  KhorConfig prev = config_snapshot();
  KhorConfig next = prev;
  next.audio_device = device;

  {
    std::scoped_lock lk(cfg_mu_);
    cfg_ = next;
  }
  (void)save_config_file(config_path_, next, nullptr);

  density_.store(next.density);
  smoothing_.store(next.smoothing);

  if (next.enable_audio) {
    std::scoped_lock lk(audio_mu_);
    (void)restart_audio_locked(next, err);
  }

  return true;
}

bool App::api_put_config(const JsonValue& patch, JsonValue* out, int* http_status) {
  if (!out) return false;
  if (!patch.is_object()) {
    if (http_status) *http_status = 400;
    *out = json_error("config patch must be a JSON object");
    return true;
  }

  KhorConfig prev = config_snapshot();
  KhorConfig next = prev;

  std::string parse_err;
  if (!config_from_json(patch, &next, &parse_err)) {
    if (http_status) *http_status = 400;
    *out = json_error(parse_err.empty() ? "invalid config patch" : parse_err);
    return true;
  }

  bool restart_required = false;
  restart_required |= (prev.listen_host != next.listen_host) || (prev.listen_port != next.listen_port);
  restart_required |= (prev.ui_dir != next.ui_dir) || (prev.serve_ui != next.serve_ui);

  // Live apply: always.
  metrics_.bpm.store(next.bpm);
  metrics_.key_midi.store(next.key_midi);
  density_.store(next.density);
  smoothing_.store(next.smoothing);

  // ---- Audio ----
  {
    std::scoped_lock lk(audio_mu_);
    audio_.set_master_gain(next.audio_master_gain);

    const bool audio_enable_changed = (prev.enable_audio != next.enable_audio);
    const bool audio_restart_needed =
      (prev.audio_backend != next.audio_backend) ||
      (prev.audio_sample_rate != next.audio_sample_rate) ||
      (prev.audio_device != next.audio_device);

    if (audio_enable_changed) {
      if (next.enable_audio) (void)start_audio_locked(next, nullptr);
      else stop_audio_locked();
    } else if (next.enable_audio && audio_restart_needed) {
      (void)restart_audio_locked(next, nullptr);
    }
  }

  // ---- MIDI ----
  {
    std::scoped_lock lk(midi_mu_);
    const bool enable_changed = (prev.enable_midi != next.enable_midi) ||
      (prev.midi_port != next.midi_port) || (prev.midi_channel != next.midi_channel);
    if (enable_changed) {
      stop_midi_locked();
      if (next.enable_midi) (void)start_midi_locked(next, nullptr);
    }
  }

  // ---- OSC ----
  {
    std::scoped_lock lk(osc_mu_);
    const bool enable_changed = (prev.enable_osc != next.enable_osc) ||
      (prev.osc_host != next.osc_host) || (prev.osc_port != next.osc_port);
    if (enable_changed) {
      stop_osc_locked();
      if (next.enable_osc) (void)start_osc_locked(next, nullptr);
    }
  }

  // ---- BPF ----
  {
    std::scoped_lock lk(bpf_mu_);
    const bool enable_changed = (prev.enable_bpf != next.enable_bpf);
    if (enable_changed) {
      stop_bpf_locked();
      if (next.enable_bpf) (void)start_bpf_locked(next, nullptr);
    } else if (next.enable_bpf) {
      // Mask/interval/filters are live-tunable.
      if (prev.bpf_enabled_mask != next.bpf_enabled_mask ||
          prev.bpf_sample_interval_ms != next.bpf_sample_interval_ms ||
          prev.bpf_tgid_allow != next.bpf_tgid_allow ||
          prev.bpf_tgid_deny != next.bpf_tgid_deny ||
          prev.bpf_cgroup_id != next.bpf_cgroup_id) {
        apply_bpf_cfg_locked(next);
      }
    }
  }

  // ---- Fake mode ----
  {
    const bool want_fake = next.enable_fake && !bpf_.status().ok;
    if (want_fake) {
      if (!fake_running_.load()) {
        if (fake_.joinable()) fake_.join();
        fake_running_.store(true);
        fake_ = std::thread([this] { fake_loop(); });
      }
    } else {
      fake_running_.store(false);
      if (fake_.joinable()) fake_.join();
    }
  }

  // Save config + publish.
  {
    std::scoped_lock lk(cfg_mu_);
    cfg_ = next;
  }

  (void)save_config_file(config_path_, next, nullptr);

  JsonValue v = config_to_json(next);
  v.o["ok"] = JsonValue::make_bool(true);
  v.o["restart_required"] = JsonValue::make_bool(restart_required);
  *out = std::move(v);
  if (http_status) *http_status = 200;
  return true;
}

} // namespace khor
