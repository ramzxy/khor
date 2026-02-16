#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include "app/app.h"
#include "app/config.h"
#include "http/server.h"
#include "util/paths.h"

namespace {

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
    "  --midi                    Enable MIDI output (ALSA sequencer)\n"
    "  --osc                     Enable OSC output (UDP)\n"
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

static bool parse_args(int argc, char** argv, Cli* out, std::string* err) {
  if (!out) return false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i] ? argv[i] : "";
    if (a == "--help" || a == "-h") { out->help = true; return true; }
    if (a == "--config") {
      if (i + 1 >= argc) { if (err) *err = "--config requires a path"; return false; }
      out->config_path = argv[++i];
      continue;
    }
    if (a == "--listen") {
      if (i + 1 >= argc) { if (err) *err = "--listen requires HOST:PORT"; return false; }
      out->listen = std::string(argv[++i]);
      continue;
    }
    if (a == "--ui-dir") {
      if (i + 1 >= argc) { if (err) *err = "--ui-dir requires a path"; return false; }
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

static volatile std::sig_atomic_t g_stop = 0;

} // namespace

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

  const std::string config_path =
    !cli.config_path.empty() ? cli.config_path : khor::path_default_config_file();

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

  khor::App app(config_path, cfg);
  std::string app_err;
  (void)app.start(&app_err);
  if (!app_err.empty()) std::fprintf(stderr, "khor-daemon: warning: %s\n", app_err.c_str());

  khor::HttpServer http(&app);
  std::string http_err;
  if (!http.start(cfg.listen_host, cfg.listen_port, cfg.ui_dir, cfg.serve_ui, &http_err)) {
    std::fprintf(stderr, "khor-daemon: http start failed: %s\n", http_err.c_str());
    return 2;
  }

  std::signal(SIGINT, +[](int) { g_stop = 1; });
  std::signal(SIGTERM, +[](int) { g_stop = 1; });

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  http.stop();
  app.stop();
  return 0;
}

