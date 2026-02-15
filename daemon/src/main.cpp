#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <optional>
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
    "khor-daemon\\n"
    "\\n"
    "Usage:\\n"
    "  %s [options]\\n"
    "\\n"
    "Options:\\n"
    "  --help, -h                Show this help\\n"
    "  --config PATH             Config file path (default: XDG config path)\\n"
    "  --listen HOST:PORT        Override listen address\\n"
    "  --ui-dir PATH             Serve UI from this directory (static)\\n"
    "  --no-bpf                  Disable eBPF collector\\n"
    "  --no-audio                Disable audio output\\n"
    "  --midi                    Enable MIDI output (optional)\\n"
    "  --osc                     Enable OSC output (optional)\\n"
    "  --fake                    Enable fake metrics mode when BPF is unavailable\\n"
    "\\n",
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
  int octave = (int)((seed / 5) % 3); // spread across 3 octaves
  return key_midi + kScale[degree] + octave * 12;
}

} // namespace

static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  KhorMetrics metrics;

  struct Cleanup {
    std::atomic<bool>* running{};
    std::thread* music{};
    std::thread* http_thread{};
    httplib::Server* http{};
    KhorAudio* audio{};
    ~Cleanup() {
      if (running) running->store(false);
      if (http) http->stop();
      if (music && music->joinable()) music->join();
      if (http_thread && http_thread->joinable()) http_thread->join();
      if (audio) {
        khor_audio_stop(audio);
        khor_audio_destroy(audio);
      }
    }
  };

  // Start audio.
  KhorAudio* audio = khor_audio_create();
  if (!khor_audio_start(audio)) {
    std::fprintf(stderr, "audio init failed\n");
    return 1;
  }

  std::atomic<bool> running{true};
  std::signal(SIGINT, +[](int){ g_stop = 1; });

  // Simple “music” loop: for now, just tick from metrics deltas.
  std::thread music([&]{
    using clock = std::chrono::steady_clock;
    uint64_t last_exec = 0;
    uint64_t last_rx = 0;
    uint64_t last_tx = 0;

    while (running.load()) {
      double bpm = metrics.bpm.load();
      if (!(bpm > 1.0 && bpm < 400.0)) bpm = 110.0;
      // 16th-note tick.
      double tick_ms = 60000.0 / bpm / 4.0;
      tick_ms = std::clamp(tick_ms, 30.0, 500.0);
      std::this_thread::sleep_for(std::chrono::milliseconds((int)tick_ms));

      uint64_t exec = metrics.exec_total.load();
      uint64_t rx = metrics.net_rx_bytes_total.load();
      uint64_t tx = metrics.net_tx_bytes_total.load();

      uint64_t d_exec = exec - last_exec;
      uint64_t d_rx = rx - last_rx;
      uint64_t d_tx = tx - last_tx;

      last_exec = exec;
      last_rx = rx;
      last_tx = tx;

      // Map deltas to notes (pentatonic, always consonant).
      const int key = metrics.key_midi.load();
      if (d_exec) {
        // Exec triggers a short chord-ish stab.
        int root = pick_scale_note(key, exec);
        const double dur = std::clamp(tick_ms / 1000.0 * 1.6, 0.08, 0.4);
        khor_audio_note_on(audio, root, 0.6f, dur);
        khor_audio_note_on(audio, root + 7, 0.45f, dur);
        khor_audio_note_on(audio, root + 12, 0.35f, dur);
      }
      if (d_rx) {
        int n = pick_scale_note(key, rx);
        float vel = std::min(1.0f, 0.2f + (float)std::log10(1.0 + (double)d_rx) * 0.15f);
        const double dur = std::clamp(tick_ms / 1000.0 * 1.2, 0.06, 0.3);
        khor_audio_note_on(audio, n, vel, dur);
      }
      if (d_tx) {
        int n = pick_scale_note(key - 12, tx);
        float vel = std::min(1.0f, 0.2f + (float)std::log10(1.0 + (double)d_tx) * 0.15f);
        const double dur = std::clamp(tick_ms / 1000.0 * 1.0, 0.05, 0.22);
        khor_audio_note_on(audio, n, vel, dur);
      }
    }
  });

  // HTTP API for UI.
  httplib::Server http;
  auto cors = [](httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
  };

  http.Options(R"(.*)", [&](const httplib::Request&, httplib::Response& res) {
    cors(res);
    res.status = 204;
  });

  http.Get("/metrics", [&](const httplib::Request&, httplib::Response& res) {
    cors(res);
    char buf[512];
    std::snprintf(
      buf, sizeof(buf),
      "{"
      "\"events_total\":%llu,"
      "\"events_dropped\":%llu,"
      "\"exec_total\":%llu,"
      "\"net_rx_bytes_total\":%llu,"
      "\"net_tx_bytes_total\":%llu,"
      "\"bpm\":%.2f,"
      "\"key_midi\":%d"
      "}\n",
      (unsigned long long)metrics.events_total.load(),
      (unsigned long long)metrics.events_dropped.load(),
      (unsigned long long)metrics.exec_total.load(),
      (unsigned long long)metrics.net_rx_bytes_total.load(),
      (unsigned long long)metrics.net_tx_bytes_total.load(),
      metrics.bpm.load(),
      metrics.key_midi.load()
    );
    res.set_content(buf, "application/json");
  });

  http.Post("/control", [&](const httplib::Request& req, httplib::Response& res) {
    cors(res);
    // MVP: accept `bpm` and `key_midi` via query params (simple, UI can call it easily).
    if (req.has_param("bpm")) metrics.bpm.store(std::atof(req.get_param_value("bpm").c_str()));
    if (req.has_param("key_midi")) metrics.key_midi.store(std::atoi(req.get_param_value("key_midi").c_str()));
    res.set_content("{\"ok\":true}\n", "application/json");
  });

  http.Post("/test/note", [&](const httplib::Request& req, httplib::Response& res) {
    cors(res);
    int midi = 62;
    float vel = 0.7f;
    double dur = 0.25;
    if (req.has_param("midi")) midi = std::atoi(req.get_param_value("midi").c_str());
    if (req.has_param("vel")) vel = (float)std::atof(req.get_param_value("vel").c_str());
    if (req.has_param("dur")) dur = std::atof(req.get_param_value("dur").c_str());
    khor_audio_note_on(audio, midi, vel, dur);
    res.set_content("{\"ok\":true}\n", "application/json");
  });

  std::thread http_thread([&]{
    std::fprintf(stderr, "khor-daemon: listening on http://127.0.0.1:17321\n");
    http.listen("127.0.0.1", 17321);
  });

  Cleanup cleanup{&running, &music, &http_thread, &http, audio};

  bool use_bpf = false;
#if defined(KHOR_HAS_BPF)
  ring_buffer* rb = nullptr;
  khor_bpf* skel = nullptr;

  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
  const bool debug_libbpf = (std::getenv("KHOR_DEBUG_LIBBPF") != nullptr);
  if (debug_libbpf) {
    libbpf_set_print([](enum libbpf_print_level, const char* fmt, va_list args) {
      return std::vfprintf(stderr, fmt, args);
    });
  } else {
    // Keep stderr clean by default, but still allow opt-in diagnostics.
    libbpf_set_print([](enum libbpf_print_level, const char*, va_list) { return 0; });
  }

  skel = khor_bpf__open();
  if (!skel) {
    std::fprintf(stderr, "failed to open BPF skeleton\n");
  } else if (khor_bpf__load(skel)) {
    std::fprintf(stderr, "failed to load BPF skeleton (need root/CAP_BPF)\n");
  } else if (khor_bpf__attach(skel)) {
    std::fprintf(stderr, "failed to attach BPF programs\n");
  } else {
    auto on_event = [](void* ctx, void* data, size_t) -> int {
      auto* m = (KhorMetrics*)ctx;
      auto* e = (const khor_event*)data;
      m->events_total.fetch_add(1);
      switch (e->type) {
        case KHOR_EV_EXEC: m->exec_total.fetch_add(1); break;
        case KHOR_EV_NET_RX: m->net_rx_bytes_total.fetch_add(e->a); break;
        case KHOR_EV_NET_TX: m->net_tx_bytes_total.fetch_add(e->a); break;
        default: break;
      }
      return 0;
    };

    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, &metrics, nullptr);
    if (!rb) {
      std::fprintf(stderr, "failed to create ring buffer\n");
    } else {
      use_bpf = true;
      std::fprintf(stderr, "khor-daemon: eBPF enabled\n");
    }
  }

  if (!use_bpf) {
    if (skel) khor_bpf__destroy(skel);
    skel = nullptr;
  }
#endif

  if (!use_bpf) {
    std::fprintf(stderr, "khor-daemon: continuing without eBPF (fake metrics mode)\n");
    std::thread fake([&]{
      while (running.load() && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        metrics.exec_total.fetch_add(1);
        metrics.net_rx_bytes_total.fetch_add(1000 + (std::rand() % 60000));
        metrics.net_tx_bytes_total.fetch_add(1000 + (std::rand() % 40000));
      }
    });
    fake.detach();
  }

  while (running.load() && !g_stop) {
    if (use_bpf) {
#if defined(KHOR_HAS_BPF)
      int err = ring_buffer__poll(rb, 50 /* ms */);
      if (err < 0) {
        metrics.events_dropped.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  running.store(false);

#if defined(KHOR_HAS_BPF)
  if (use_bpf) {
    ring_buffer__free(rb);
    khor_bpf__destroy(skel);
  }
#endif
  return 0;
}
