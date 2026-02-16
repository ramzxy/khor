#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

#include "app/config.h"
#include "khor/audio.h"
#include "khor/metrics.h"
#include "util/json.h"
#include "util/paths.h"
#include "../bpf/khor.h"

#if defined(KHOR_HAS_BPF)
#include "khor.skel.h"
#include <bpf/libbpf.h>
#endif

namespace {

static int64_t unix_ms_now() {
  using clock = std::chrono::system_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
}

static void print_help(const char* argv0) {
  std::fprintf(stderr,
    "khor-daemon\n"
    "\n"
    "Usage:\n"
    "  %s [options]\n"
    "\n"
    "Options:\n"
    "  --help, -h                Show this help\n"
    "  --config PATH             Config file path (default: XDG config path)\n"
    "  --listen HOST:PORT        Override listen address\n"
    "  --ui-dir PATH             Serve UI from this directory (static)\n"
    "  --no-bpf                  Disable eBPF collector\n"
    "  --no-audio                Disable audio output\n"
    "  --midi                    Enable MIDI output (optional, TODO)\n"
    "  --osc                     Enable OSC output (optional, TODO)\n"
    "  --fake                    Enable fake metrics mode when BPF is unavailable\n"
    "\n",
    argv0 ? argv0 : "khor-daemon"
  );
}

static bool parse_listen(const std::string& s, std::string* host, int* port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  std::string h = s.substr(0, pos);
  std::string p = s.substr(pos + 1);
  if (h.empty() || p.empty()) return false;
  char* endp = nullptr;
  long v = std::strtol(p.c_str(), &endp, 10);
  if (!endp || *endp != 0) return false;
  if (v < 1 || v > 65535) return false;
  *host = std::move(h);
  *port = (int)v;
  return true;
}

static bool read_file(const std::string& path, std::string* out) {
  if (!out) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

struct Cli {
  bool help = false;
  std::string config_path;

  std::optional<std::string> listen;
  std::optional<std::string> ui_dir;

  std::optional<bool> enable_bpf;
  std::optional<bool> enable_audio;
  std::optional<bool> enable_midi;
  std::optional<bool> enable_osc;
  std::optional<bool> enable_fake;
};

static bool parse_args(int argc, char** argv, Cli* out, std::string* err) {
  if (!out) return false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i] ? argv[i] : "";
    if (a == "--help" || a == "-h") {
      out->help = true;
      return true;
    }
    if (a == "--config") {
      if (i + 1 >= argc) {
        if (err) *err = "--config requires a path";
        return false;
      }
      out->config_path = argv[++i];
      continue;
    }
    if (a == "--listen") {
      if (i + 1 >= argc) {
        if (err) *err = "--listen requires HOST:PORT";
        return false;
      }
      out->listen = std::string(argv[++i]);
      continue;
    }
    if (a == "--ui-dir") {
      if (i + 1 >= argc) {
        if (err) *err = "--ui-dir requires a path";
        return false;
      }
      out->ui_dir = std::string(argv[++i]);
      continue;
    }
    if (a == "--no-bpf") { out->enable_bpf = false; continue; }
    if (a == "--no-audio") { out->enable_audio = false; continue; }
    if (a == "--midi") { out->enable_midi = true; continue; }
    if (a == "--osc") { out->enable_osc = true; continue; }
    if (a == "--fake") { out->enable_fake = true; continue; }

    if (err) *err = "unknown argument: " + a;
    return false;
  }
  return true;
}

// D minor pentatonic: D F G A C (safe default).
constexpr int kScale[5] = {0, 3, 5, 7, 10};

static int pick_scale_note(int key_midi, uint64_t seed) {
  int degree = (int)(seed % 5);
  int octave = (int)((seed / 5) % 3);
  return key_midi + kScale[degree] + octave * 12;
}

} // namespace

static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char** argv) {
  Cli cli;
  std::string arg_err;
  if (!parse_args(argc, argv, &cli, &arg_err)) {
    std::fprintf(stderr, "%s\n", arg_err.c_str());
    print_help(argv[0]);
    return 2;
  }
  if (cli.help) {
    print_help(argv[0]);
    return 0;
  }

  const std::string config_path = !cli.config_path.empty() ? cli.config_path : khor::path_default_config_file();

  khor::KhorConfig cfg;
  cfg.ui_dir = khor::path_default_ui_dir();

  std::string cfg_err;
  if (!khor::load_config_file(config_path, &cfg, &cfg_err)) {
    std::fprintf(stderr, "config load failed (%s): %s\n", config_path.c_str(), cfg_err.c_str());
    return 2;
  }
  if (cfg.ui_dir.empty()) cfg.ui_dir = khor::path_default_ui_dir();

  if (cli.listen) {
    if (!parse_listen(*cli.listen, &cfg.listen_host, &cfg.listen_port)) {
      std::fprintf(stderr, "invalid --listen (expected HOST:PORT): %s\n", cli.listen->c_str());
      return 2;
    }
  }
  if (cli.ui_dir) cfg.ui_dir = *cli.ui_dir;
  if (cli.enable_bpf) cfg.enable_bpf = *cli.enable_bpf;
  if (cli.enable_audio) cfg.enable_audio = *cli.enable_audio;
  if (cli.enable_midi) cfg.enable_midi = *cli.enable_midi;
  if (cli.enable_osc) cfg.enable_osc = *cli.enable_osc;
  if (cli.enable_fake) cfg.enable_fake = *cli.enable_fake;

  std::mutex cfg_mu;

  KhorMetrics metrics;
  metrics.bpm.store(cfg.bpm);
  metrics.key_midi.store(cfg.key_midi);

  std::atomic<double> density{cfg.density};
  std::atomic<double> smoothing{cfg.smoothing};

  std::atomic<bool> running{true};
  std::signal(SIGINT, +[](int){ g_stop = 1; });
  std::signal(SIGTERM, +[](int){ g_stop = 1; });

  // ---- Audio ----
  KhorAudio* audio = nullptr;
  std::atomic<bool> audio_ok{false};
  std::mutex audio_err_mu;
  std::string audio_err;

  if (cfg.enable_audio) {
    if (!cfg.audio_backend.empty()) {
      ::setenv("KHOR_AUDIO_BACKEND", cfg.audio_backend.c_str(), /*overwrite=*/1);
    }
    audio = khor_audio_create();
    if (!khor_audio_start(audio)) {
      std::scoped_lock lk(audio_err_mu);
      audio_err = "audio init failed";
      khor_audio_destroy(audio);
      audio = nullptr;
    } else {
      audio_ok.store(true);
    }
  }

  // ---- Rates + history sampler ----
  struct Totals {
    uint64_t events_total = 0;
    uint64_t events_dropped = 0;
    uint64_t exec_total = 0;
    uint64_t net_rx_bytes_total = 0;
    uint64_t net_tx_bytes_total = 0;
    uint64_t sched_switch_total = 0;
    uint64_t blk_read_bytes_total = 0;
    uint64_t blk_write_bytes_total = 0;
  };

  auto read_totals = [&]() -> Totals {
    Totals t;
    t.events_total = metrics.events_total.load();
    t.events_dropped = metrics.events_dropped.load();
    t.exec_total = metrics.exec_total.load();
    t.net_rx_bytes_total = metrics.net_rx_bytes_total.load();
    t.net_tx_bytes_total = metrics.net_tx_bytes_total.load();
    t.sched_switch_total = metrics.sched_switch_total.load();
    t.blk_read_bytes_total = metrics.blk_read_bytes_total.load();
    t.blk_write_bytes_total = metrics.blk_write_bytes_total.load();
    return t;
  };

  struct RateAtoms {
    std::atomic<double> exec_s{0};
    std::atomic<double> rx_kbs{0};
    std::atomic<double> tx_kbs{0};
    std::atomic<double> csw_s{0};
    std::atomic<double> blk_r_kbs{0};
    std::atomic<double> blk_w_kbs{0};
  } rates;

  struct HistSample {
    int64_t ts_ms = 0;
    double exec_s = 0;
    double rx_kbs = 0;
    double tx_kbs = 0;
    double csw_s = 0;
    double blk_r_kbs = 0;
    double blk_w_kbs = 0;
  };

  std::mutex hist_mu;
  std::deque<HistSample> history;

  std::thread sampler([&] {
    using clock = std::chrono::steady_clock;
    Totals last = read_totals();
    auto last_t = clock::now();

    while (running.load() && !g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      Totals cur = read_totals();
      auto now = clock::now();
      double dt = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_t).count();
      if (dt <= 0.0) dt = 0.1;

      const double exec_s = (double)(cur.exec_total - last.exec_total) / dt;
      const double rx_kbs = (double)(cur.net_rx_bytes_total - last.net_rx_bytes_total) / dt / 1024.0;
      const double tx_kbs = (double)(cur.net_tx_bytes_total - last.net_tx_bytes_total) / dt / 1024.0;
      const double csw_s = (double)(cur.sched_switch_total - last.sched_switch_total) / dt;
      const double blk_r_kbs = (double)(cur.blk_read_bytes_total - last.blk_read_bytes_total) / dt / 1024.0;
      const double blk_w_kbs = (double)(cur.blk_write_bytes_total - last.blk_write_bytes_total) / dt / 1024.0;

      rates.exec_s.store(exec_s);
      rates.rx_kbs.store(rx_kbs);
      rates.tx_kbs.store(tx_kbs);
      rates.csw_s.store(csw_s);
      rates.blk_r_kbs.store(blk_r_kbs);
      rates.blk_w_kbs.store(blk_w_kbs);

      {
        std::scoped_lock lk(hist_mu);
        history.push_back(HistSample{
          .ts_ms = unix_ms_now(),
          .exec_s = exec_s,
          .rx_kbs = rx_kbs,
          .tx_kbs = tx_kbs,
          .csw_s = csw_s,
          .blk_r_kbs = blk_r_kbs,
          .blk_w_kbs = blk_w_kbs,
        });
        while (history.size() > 600) history.pop_front();
      }

      last = cur;
      last_t = now;
    }
  });

  // ---- Music (still MVP mapping; v1.0 engine will replace this) ----
  std::thread music([&] {
    uint64_t last_exec = 0;
    uint64_t last_rx = 0;
    uint64_t last_tx = 0;

    while (running.load() && !g_stop) {
      double bpm = metrics.bpm.load();
      if (!(bpm > 1.0 && bpm < 400.0)) bpm = 110.0;
      double tick_ms = 60000.0 / bpm / 4.0; // 16th note
      tick_ms = std::clamp(tick_ms, 30.0, 500.0);
      std::this_thread::sleep_for(std::chrono::milliseconds((int)tick_ms));

      if (!audio) continue;

      uint64_t exec = metrics.exec_total.load();
      uint64_t rx = metrics.net_rx_bytes_total.load();
      uint64_t tx = metrics.net_tx_bytes_total.load();

      uint64_t d_exec = exec - last_exec;
      uint64_t d_rx = rx - last_rx;
      uint64_t d_tx = tx - last_tx;

      last_exec = exec;
      last_rx = rx;
      last_tx = tx;

      const double dens = std::clamp(density.load(), 0.0, 1.0);
      const int key = metrics.key_midi.load();

      if (d_exec && dens > 0.05) {
        int root = pick_scale_note(key, exec);
        const double dur = std::clamp(tick_ms / 1000.0 * (1.0 + dens), 0.06, 0.5);
        khor_audio_note_on(audio, root, 0.55f, dur);
        khor_audio_note_on(audio, root + 7, 0.38f, dur);
        khor_audio_note_on(audio, root + 12, 0.28f, dur);
      }
      if (d_rx && dens > 0.01) {
        int n = pick_scale_note(key, rx);
        float vel = std::min(1.0f, 0.15f + (float)std::log10(1.0 + (double)d_rx) * (0.10f + (float)dens * 0.25f));
        const double dur = std::clamp(tick_ms / 1000.0 * (0.9 + dens), 0.05, 0.35);
        khor_audio_note_on(audio, n, vel, dur);
      }
      if (d_tx && dens > 0.01) {
        int n = pick_scale_note(key - 12, tx);
        float vel = std::min(1.0f, 0.15f + (float)std::log10(1.0 + (double)d_tx) * (0.10f + (float)dens * 0.25f));
        const double dur = std::clamp(tick_ms / 1000.0 * (0.8 + dens), 0.05, 0.28);
        khor_audio_note_on(audio, n, vel, dur);
      }
    }
  });

  // ---- eBPF ----
  std::atomic<bool> bpf_ok{false};
  std::mutex bpf_err_mu;
  std::string bpf_err;

#if defined(KHOR_HAS_BPF)
  ring_buffer* rb = nullptr;
  khor_bpf* skel = nullptr;

  if (cfg.enable_bpf) {
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

    const bool debug_libbpf = (std::getenv("KHOR_DEBUG_LIBBPF") != nullptr);
    if (debug_libbpf) {
      libbpf_set_print([](enum libbpf_print_level, const char* fmt, va_list args) {
        return std::vfprintf(stderr, fmt, args);
      });
    } else {
      libbpf_set_print([](enum libbpf_print_level, const char*, va_list) { return 0; });
    }

    skel = khor_bpf__open();
    if (!skel) {
      std::scoped_lock lk(bpf_err_mu);
      bpf_err = "failed to open BPF skeleton";
    } else if (khor_bpf__load(skel)) {
      std::scoped_lock lk(bpf_err_mu);
      bpf_err = "failed to load BPF skeleton (need CAP_BPF/CAP_PERFMON or root)";
    } else if (khor_bpf__attach(skel)) {
      std::scoped_lock lk(bpf_err_mu);
      bpf_err = "failed to attach BPF programs";
    } else {
      auto on_event = [](void* ctx, void* data, size_t) -> int {
        auto* m = (KhorMetrics*)ctx;
        auto* e = (const khor_event*)data;
        m->events_total.fetch_add(1);
        switch (e->type) {
          case KHOR_EV_SAMPLE:
            m->exec_total.fetch_add(e->u.sample.exec_count);
            m->net_rx_bytes_total.fetch_add(e->u.sample.net_rx_bytes);
            m->net_tx_bytes_total.fetch_add(e->u.sample.net_tx_bytes);
            m->sched_switch_total.fetch_add(e->u.sample.sched_switches);
            m->blk_read_bytes_total.fetch_add(e->u.sample.blk_read_bytes);
            m->blk_write_bytes_total.fetch_add(e->u.sample.blk_write_bytes);
            break;
          default: break;
        }
        return 0;
      };

      rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, &metrics, nullptr);
      if (!rb) {
        std::scoped_lock lk(bpf_err_mu);
        bpf_err = "failed to create ring buffer";
      } else {
        bpf_ok.store(true);
        std::fprintf(stderr, "khor-daemon: eBPF enabled\n");
      }
    }
  } else {
    std::scoped_lock lk(bpf_err_mu);
    bpf_err = "disabled by config";
  }
#else
  {
    std::scoped_lock lk(bpf_err_mu);
    bpf_err = "built without eBPF support";
  }
#endif

  std::thread fake_thread;
  if (!bpf_ok.load() && cfg.enable_fake) {
    std::fprintf(stderr, "khor-daemon: fake metrics enabled\n");
    fake_thread = std::thread([&] {
      while (running.load() && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        metrics.exec_total.fetch_add(1);
        metrics.net_rx_bytes_total.fetch_add(1000 + (std::rand() % 60000));
        metrics.net_tx_bytes_total.fetch_add(1000 + (std::rand() % 40000));
        metrics.sched_switch_total.fetch_add(5 + (std::rand() % 200));
        metrics.blk_read_bytes_total.fetch_add(4096 * (std::rand() % 8));
        metrics.blk_write_bytes_total.fetch_add(4096 * (std::rand() % 6));
      }
    });
  }

  std::thread bpf_thread;
#if defined(KHOR_HAS_BPF)
  if (bpf_ok.load()) {
    bpf_thread = std::thread([&] {
      while (running.load() && !g_stop) {
        int err = ring_buffer__poll(rb, 50 /* ms */);
        if (err < 0) {
          metrics.events_dropped.fetch_add(1);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    });
  } else {
    if (skel) khor_bpf__destroy(skel);
    skel = nullptr;
  }
#endif

  // ---- HTTP + UI ----
  httplib::Server http;

  auto json_reply = [](httplib::Response& res, const khor::JsonValue& v) {
    std::string body = khor::json_stringify(v, 0);
    res.set_content(body, "application/json");
  };

  auto metrics_json = [&](bool include_history) -> khor::JsonValue {
    Totals t = read_totals();

    khor::JsonValue root = khor::JsonValue::make_object({});
    root.o["ts_ms"] = khor::JsonValue::make_number((double)unix_ms_now());

    root.o["totals"] = khor::JsonValue::make_object({
      {"events_total", khor::JsonValue::make_number((double)t.events_total)},
      {"events_dropped", khor::JsonValue::make_number((double)t.events_dropped)},
      {"exec_total", khor::JsonValue::make_number((double)t.exec_total)},
      {"net_rx_bytes_total", khor::JsonValue::make_number((double)t.net_rx_bytes_total)},
      {"net_tx_bytes_total", khor::JsonValue::make_number((double)t.net_tx_bytes_total)},
      {"sched_switch_total", khor::JsonValue::make_number((double)t.sched_switch_total)},
      {"blk_read_bytes_total", khor::JsonValue::make_number((double)t.blk_read_bytes_total)},
      {"blk_write_bytes_total", khor::JsonValue::make_number((double)t.blk_write_bytes_total)},
    });

    root.o["rates"] = khor::JsonValue::make_object({
      {"exec_s", khor::JsonValue::make_number(rates.exec_s.load())},
      {"rx_kbs", khor::JsonValue::make_number(rates.rx_kbs.load())},
      {"tx_kbs", khor::JsonValue::make_number(rates.tx_kbs.load())},
      {"csw_s", khor::JsonValue::make_number(rates.csw_s.load())},
      {"blk_r_kbs", khor::JsonValue::make_number(rates.blk_r_kbs.load())},
      {"blk_w_kbs", khor::JsonValue::make_number(rates.blk_w_kbs.load())},
    });

    root.o["controls"] = khor::JsonValue::make_object({
      {"bpm", khor::JsonValue::make_number(metrics.bpm.load())},
      {"key_midi", khor::JsonValue::make_number(metrics.key_midi.load())},
      {"density", khor::JsonValue::make_number(density.load())},
      {"smoothing", khor::JsonValue::make_number(smoothing.load())},
    });

    if (include_history) {
      std::vector<khor::JsonValue> arr;
      {
        std::scoped_lock lk(hist_mu);
        arr.reserve(history.size());
        for (const auto& s : history) {
          khor::JsonValue o = khor::JsonValue::make_object({});
          o.o["ts_ms"] = khor::JsonValue::make_number((double)s.ts_ms);
          o.o["exec_s"] = khor::JsonValue::make_number(s.exec_s);
          o.o["rx_kbs"] = khor::JsonValue::make_number(s.rx_kbs);
          o.o["tx_kbs"] = khor::JsonValue::make_number(s.tx_kbs);
          o.o["csw_s"] = khor::JsonValue::make_number(s.csw_s);
          o.o["blk_r_kbs"] = khor::JsonValue::make_number(s.blk_r_kbs);
          o.o["blk_w_kbs"] = khor::JsonValue::make_number(s.blk_w_kbs);
          arr.push_back(std::move(o));
        }
      }
      root.o["history"] = khor::JsonValue::make_array(std::move(arr));
    }

    return root;
  };

  http.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
    khor::KhorConfig cfg_snapshot;
    {
      std::scoped_lock lk(cfg_mu);
      cfg_snapshot = cfg;
    }

    khor::JsonValue root = khor::JsonValue::make_object({});
    root.o["ts_ms"] = khor::JsonValue::make_number((double)unix_ms_now());
    root.o["config_path"] = khor::JsonValue::make_string(config_path);

    root.o["audio"] = khor::JsonValue::make_object({
      {"enabled", khor::JsonValue::make_bool(cfg_snapshot.enable_audio)},
      {"ok", khor::JsonValue::make_bool(audio_ok.load())},
      {"backend", khor::JsonValue::make_string(cfg_snapshot.audio_backend.empty() ? "auto" : cfg_snapshot.audio_backend)},
    });

    {
      std::scoped_lock lk(audio_err_mu);
      if (!audio_err.empty()) root.o["audio"].o["error"] = khor::JsonValue::make_string(audio_err);
    }

    root.o["bpf"] = khor::JsonValue::make_object({
      {"enabled", khor::JsonValue::make_bool(cfg_snapshot.enable_bpf)},
      {"ok", khor::JsonValue::make_bool(bpf_ok.load())},
    });

    {
      std::scoped_lock lk(bpf_err_mu);
      root.o["bpf"].o["error"] = khor::JsonValue::make_string(bpf_err);
    }

    root.o["features"] = khor::JsonValue::make_object({
      {"midi", khor::JsonValue::make_bool(cfg_snapshot.enable_midi)},
      {"osc", khor::JsonValue::make_bool(cfg_snapshot.enable_osc)},
      {"fake", khor::JsonValue::make_bool(cfg_snapshot.enable_fake)},
    });

    json_reply(res, root);
  });

  http.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, metrics_json(/*include_history=*/true));
  });

  http.Get("/api/config", [&](const httplib::Request&, httplib::Response& res) {
    std::scoped_lock lk(cfg_mu);
    json_reply(res, khor::config_to_json(cfg));
  });

  http.Put("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
    khor::JsonValue body;
    khor::JsonParseError perr;
    if (!khor::json_parse(req.body, &body, &perr)) {
      res.status = 400;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
        {"error", khor::JsonValue::make_string("invalid JSON body")},
      }));
      return;
    }

    khor::KhorConfig prev;
    {
      std::scoped_lock lk(cfg_mu);
      prev = cfg;
    }

    khor::KhorConfig next = prev;
    std::string parse_err;
    if (!khor::config_from_json(body, &next, &parse_err)) {
      res.status = 400;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
        {"error", khor::JsonValue::make_string(parse_err.empty() ? "invalid config" : parse_err)},
      }));
      return;
    }

    bool restart_required = false;
    restart_required |= (prev.listen_host != next.listen_host) || (prev.listen_port != next.listen_port);
    restart_required |= (prev.ui_dir != next.ui_dir) || (prev.serve_ui != next.serve_ui);
    restart_required |= (prev.enable_bpf != next.enable_bpf);
    restart_required |= (prev.enable_audio != next.enable_audio) || (prev.audio_backend != next.audio_backend) || (prev.audio_device != next.audio_device);
    restart_required |= (prev.enable_midi != next.enable_midi) || (prev.enable_osc != next.enable_osc);

    {
      std::scoped_lock lk(cfg_mu);
      cfg = next;
    }

    // Live-apply safe controls.
    metrics.bpm.store(next.bpm);
    metrics.key_midi.store(next.key_midi);
    density.store(next.density);
    smoothing.store(next.smoothing);

    std::string save_err;
    if (!khor::save_config_file(config_path, next, &save_err)) {
      std::fprintf(stderr, "warning: failed to save config: %s\n", save_err.c_str());
    }

    khor::JsonValue out = khor::config_to_json(next);
    out.o["ok"] = khor::JsonValue::make_bool(true);
    out.o["restart_required"] = khor::JsonValue::make_bool(restart_required);
    json_reply(res, out);
  });

  http.Get("/api/presets", [&](const httplib::Request&, httplib::Response& res) {
    std::vector<khor::JsonValue> arr;
    arr.push_back(khor::JsonValue::make_object({{"name", khor::JsonValue::make_string("ambient")}}));
    arr.push_back(khor::JsonValue::make_object({{"name", khor::JsonValue::make_string("percussive")}}));
    arr.push_back(khor::JsonValue::make_object({{"name", khor::JsonValue::make_string("arp")}}));
    arr.push_back(khor::JsonValue::make_object({{"name", khor::JsonValue::make_string("drone")}}));
    json_reply(res, khor::JsonValue::make_object({
      {"presets", khor::JsonValue::make_array(std::move(arr))},
    }));
  });

  http.Post("/api/preset/select", [&](const httplib::Request& req, httplib::Response& res) {
    std::string name;
    if (req.has_param("name")) name = req.get_param_value("name");
    if (name.empty()) {
      res.status = 400;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
        {"error", khor::JsonValue::make_string("missing preset name")},
      }));
      return;
    }

    // Preset affects mapping params; keep bpm/key user-controlled.
    if (name == "ambient") {
      density.store(0.20);
      smoothing.store(0.92);
    } else if (name == "percussive") {
      density.store(0.80);
      smoothing.store(0.35);
    } else if (name == "arp") {
      density.store(0.55);
      smoothing.store(0.60);
    } else if (name == "drone") {
      density.store(0.10);
      smoothing.store(0.95);
    } else {
      res.status = 400;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
        {"error", khor::JsonValue::make_string("unknown preset")},
      }));
      return;
    }

    {
      std::scoped_lock lk(cfg_mu);
      cfg.preset = name;
      cfg.density = density.load();
      cfg.smoothing = smoothing.load();
      (void)khor::save_config_file(config_path, cfg, nullptr);
    }

    json_reply(res, khor::JsonValue::make_object({
      {"ok", khor::JsonValue::make_bool(true)},
    }));
  });

  http.Post("/api/actions/test_note", [&](const httplib::Request& req, httplib::Response& res) {
    if (!audio) {
      res.status = 409;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
        {"error", khor::JsonValue::make_string("audio disabled or failed")},
      }));
      return;
    }

    int midi = 62;
    float vel = 0.7f;
    double dur = 0.25;

    if (req.has_param("midi")) midi = std::atoi(req.get_param_value("midi").c_str());
    if (req.has_param("vel")) vel = (float)std::atof(req.get_param_value("vel").c_str());
    if (req.has_param("dur")) dur = std::atof(req.get_param_value("dur").c_str());

    khor_audio_note_on(audio, midi, vel, dur);
    json_reply(res, khor::JsonValue::make_object({
      {"ok", khor::JsonValue::make_bool(true)},
    }));
  });

  http.Get("/api/stream", [&](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Access-Control-Allow-Origin", "*");

    res.set_chunked_content_provider(
      "text/event-stream",
      [&](size_t, httplib::DataSink& sink) {
        while (running.load() && !g_stop && sink.is_writable()) {
          khor::JsonValue v = metrics_json(/*include_history=*/false);
          std::string payload = khor::json_stringify(v, 0);
          std::string msg = "data: " + payload + "\n\n";
          if (!sink.write(msg.c_str(), msg.size())) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sink.done();
        return true;
      }
    );
  });

  // Backwards-compatible MVP endpoints.
  http.Get("/metrics", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, metrics_json(/*include_history=*/false));
  });

  http.Post("/control", [&](const httplib::Request& req, httplib::Response& res) {
    if (req.has_param("bpm")) metrics.bpm.store(std::atof(req.get_param_value("bpm").c_str()));
    if (req.has_param("key_midi")) metrics.key_midi.store(std::atoi(req.get_param_value("key_midi").c_str()));
    json_reply(res, khor::JsonValue::make_object({
      {"ok", khor::JsonValue::make_bool(true)},
    }));
  });

  http.Post("/test/note", [&](const httplib::Request& req, httplib::Response& res) {
    if (!audio) {
      res.status = 409;
      json_reply(res, khor::JsonValue::make_object({
        {"ok", khor::JsonValue::make_bool(false)},
      }));
      return;
    }

    int midi = 62;
    float vel = 0.7f;
    double dur = 0.25;

    if (req.has_param("midi")) midi = std::atoi(req.get_param_value("midi").c_str());
    if (req.has_param("vel")) vel = (float)std::atof(req.get_param_value("vel").c_str());
    if (req.has_param("dur")) dur = std::atof(req.get_param_value("dur").c_str());

    khor_audio_note_on(audio, midi, vel, dur);
    json_reply(res, khor::JsonValue::make_object({
      {"ok", khor::JsonValue::make_bool(true)},
    }));
  });

  // UI mounting (best-effort, uses startup snapshot; changes require restart).
  const std::string ui_dir_snapshot = cfg.ui_dir;
  const bool serve_ui_snapshot = cfg.serve_ui;

  if (serve_ui_snapshot && !ui_dir_snapshot.empty() && std::filesystem::exists(ui_dir_snapshot)) {
    if (!http.set_mount_point("/", ui_dir_snapshot)) {
      std::fprintf(stderr, "khor-daemon: failed to mount ui dir: %s\n", ui_dir_snapshot.c_str());
    } else {
      std::fprintf(stderr, "khor-daemon: serving UI from %s\n", ui_dir_snapshot.c_str());
    }

    http.set_error_handler([&](const httplib::Request& req, httplib::Response& res) {
      if (req.method != "GET") return;
      if (req.path.rfind("/api", 0) == 0) return;
      std::string index_html;
      const std::string p = (std::filesystem::path(ui_dir_snapshot) / "index.html").string();
      if (read_file(p, &index_html)) {
        res.status = 200;
        res.set_content(index_html, "text/html");
      }
    });
  }

  std::thread http_thread([&] {
    std::fprintf(stderr, "khor-daemon: listening on http://%s:%d\n", cfg.listen_host.c_str(), cfg.listen_port);
    http.listen(cfg.listen_host, cfg.listen_port);
  });

  while (running.load() && !g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  running.store(false);
  http.stop();

  if (http_thread.joinable()) http_thread.join();
  if (bpf_thread.joinable()) bpf_thread.join();
  if (fake_thread.joinable()) fake_thread.join();
  if (music.joinable()) music.join();
  if (sampler.joinable()) sampler.join();

#if defined(KHOR_HAS_BPF)
  if (rb) ring_buffer__free(rb);
  if (skel) khor_bpf__destroy(skel);
#endif

  if (audio) {
    khor_audio_stop(audio);
    khor_audio_destroy(audio);
  }

  return 0;
}
