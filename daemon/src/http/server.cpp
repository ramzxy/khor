#include "http/server.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "httplib.h"

#include "app/app.h"
#include "app/config.h"
#include "util/json.h"

namespace khor {
namespace {

static bool read_file(const std::string& path, std::string* out) {
  if (!out) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static void json_reply(httplib::Response& res, const JsonValue& v) {
  std::string body = json_stringify(v, 0);
  res.set_content(body, "application/json");
}

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

struct HttpServer::Impl {
  App* app = nullptr;
  httplib::Server http;
  std::thread t;
  std::atomic<bool> running{false};

  std::string ui_dir_snapshot;
  bool serve_ui_snapshot = false;
};

HttpServer::HttpServer(App* app) : impl_(new Impl()) { impl_->app = app; }
HttpServer::~HttpServer() { stop(); delete impl_; impl_ = nullptr; }

bool HttpServer::is_running() const { return impl_ && impl_->running.load(); }

bool HttpServer::start(const std::string& host, int port, const std::string& ui_dir, bool serve_ui, std::string* err) {
  if (!impl_ || !impl_->app) return false;
  stop();

  impl_->serve_ui_snapshot = serve_ui;
  impl_->ui_dir_snapshot = ui_dir;

  // ---- Routes ----
  impl_->http.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, impl_->app->api_health());
  });

  impl_->http.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, impl_->app->api_metrics(/*include_history=*/true));
  });

  impl_->http.Get("/api/config", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, config_to_json(impl_->app->config_snapshot()));
  });

  impl_->http.Put("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
    JsonValue body;
    JsonParseError perr;
    if (!json_parse(req.body, &body, &perr)) {
      res.status = 400;
      json_reply(res, json_error("invalid JSON body"));
      return;
    }

    JsonValue out;
    int status = 200;
    (void)impl_->app->api_put_config(body, &out, &status);
    res.status = status;
    json_reply(res, out);
  });

  impl_->http.Get("/api/presets", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, impl_->app->api_presets());
  });

  impl_->http.Post("/api/preset/select", [&](const httplib::Request& req, httplib::Response& res) {
    std::string name = req.has_param("name") ? req.get_param_value("name") : "";
    if (name.empty()) {
      res.status = 400;
      json_reply(res, json_error("missing preset name"));
      return;
    }
    std::string e;
    if (!impl_->app->api_select_preset(name, &e)) {
      res.status = 400;
      json_reply(res, json_error(e.empty() ? "preset select failed" : e));
      return;
    }
    json_reply(res, json_ok(true));
  });

  impl_->http.Get("/api/audio/devices", [&](const httplib::Request&, httplib::Response& res) {
    std::vector<AudioDeviceInfo> devs;
    std::string e;
    if (!impl_->app->api_audio_devices(&devs, &e)) {
      res.status = 500;
      json_reply(res, json_error(e.empty() ? "device enumeration failed" : e));
      return;
    }
    std::vector<JsonValue> arr;
    arr.reserve(devs.size());
    for (const auto& d : devs) {
      arr.push_back(JsonValue::make_object({
        {"id", JsonValue::make_string(d.id)},
        {"name", JsonValue::make_string(d.name)},
        {"is_default", JsonValue::make_bool(d.is_default)},
      }));
    }
    json_reply(res, JsonValue::make_object({{"devices", JsonValue::make_array(std::move(arr))}}));
  });

  impl_->http.Post("/api/audio/device", [&](const httplib::Request& req, httplib::Response& res) {
    std::string device;
    if (req.has_param("device")) device = req.get_param_value("device");
    if (device.empty() && !req.body.empty()) {
      JsonValue body;
      JsonParseError perr;
      if (json_parse(req.body, &body, &perr)) {
        device = json_get_string(body, "device", "");
      }
    }
    if (device.empty()) {
      res.status = 400;
      json_reply(res, json_error("missing device"));
      return;
    }
    std::string e;
    if (!impl_->app->api_audio_set_device(device, &e)) {
      res.status = 500;
      json_reply(res, json_error(e.empty() ? "failed to set device" : e));
      return;
    }
    json_reply(res, json_ok(true));
  });

  impl_->http.Post("/api/actions/test_note", [&](const httplib::Request& req, httplib::Response& res) {
    int midi = 62;
    float vel = 0.7f;
    double dur = 0.25;
    if (req.has_param("midi")) midi = std::atoi(req.get_param_value("midi").c_str());
    if (req.has_param("vel")) vel = (float)std::atof(req.get_param_value("vel").c_str());
    if (req.has_param("dur")) dur = std::atof(req.get_param_value("dur").c_str());

    std::string e;
    if (!impl_->app->api_test_note(midi, vel, dur, &e)) {
      res.status = 409;
      json_reply(res, json_error(e.empty() ? "test_note failed" : e));
      return;
    }
    json_reply(res, json_ok(true));
  });

  impl_->http.Get("/api/stream", [&](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    res.set_chunked_content_provider(
      "text/event-stream",
      [&](size_t, httplib::DataSink& sink) {
        while (impl_->running.load() && sink.is_writable()) {
          JsonValue v = impl_->app->api_metrics(/*include_history=*/false);
          std::string payload = json_stringify(v, 0);
          std::string msg = "data: " + payload + "\n\n";
          if (!sink.write(msg.c_str(), msg.size())) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sink.done();
        return true;
      }
    );
  });

  // ---- Backwards-compatible MVP endpoints ----
  impl_->http.Get("/metrics", [&](const httplib::Request&, httplib::Response& res) {
    json_reply(res, impl_->app->api_metrics(/*include_history=*/false));
  });

  impl_->http.Post("/control", [&](const httplib::Request& req, httplib::Response& res) {
    JsonValue patch = JsonValue::make_object({});
    JsonValue music = JsonValue::make_object({});
    if (req.has_param("bpm")) music.o["bpm"] = JsonValue::make_number(std::atof(req.get_param_value("bpm").c_str()));
    if (req.has_param("key_midi")) music.o["key_midi"] = JsonValue::make_number(std::atoi(req.get_param_value("key_midi").c_str()));
    patch.o["music"] = std::move(music);
    JsonValue out;
    int status = 200;
    (void)impl_->app->api_put_config(patch, &out, &status);
    res.status = status;
    json_reply(res, out);
  });

  impl_->http.Post("/test/note", [&](const httplib::Request& req, httplib::Response& res) {
    int midi = 62;
    float vel = 0.7f;
    double dur = 0.25;
    if (req.has_param("midi")) midi = std::atoi(req.get_param_value("midi").c_str());
    if (req.has_param("vel")) vel = (float)std::atof(req.get_param_value("vel").c_str());
    if (req.has_param("dur")) dur = std::atof(req.get_param_value("dur").c_str());

    std::string e;
    if (!impl_->app->api_test_note(midi, vel, dur, &e)) {
      res.status = 409;
      json_reply(res, json_error(e.empty() ? "test_note failed" : e));
      return;
    }
    json_reply(res, json_ok(true));
  });

  // ---- UI mounting ----
  if (impl_->serve_ui_snapshot && !impl_->ui_dir_snapshot.empty() && std::filesystem::exists(impl_->ui_dir_snapshot)) {
    if (!impl_->http.set_mount_point("/", impl_->ui_dir_snapshot)) {
      std::fprintf(stderr, "khor-daemon: failed to mount ui dir: %s\n", impl_->ui_dir_snapshot.c_str());
    } else {
      std::fprintf(stderr, "khor-daemon: serving UI from %s\n", impl_->ui_dir_snapshot.c_str());
    }

    impl_->http.set_error_handler([&](const httplib::Request& req, httplib::Response& res) {
      if (req.method != "GET") return;
      if (req.path.rfind("/api", 0) == 0) return;
      std::string index_html;
      const std::string p = (std::filesystem::path(impl_->ui_dir_snapshot) / "index.html").string();
      if (read_file(p, &index_html)) {
        res.status = 200;
        res.set_content(index_html, "text/html");
      }
    });
  }

  // Bind before launching thread so we can fail fast on port-in-use.
  if (!impl_->http.bind_to_port(host.c_str(), port)) {
    if (err) *err = "failed to bind HTTP server (port in use?)";
    return false;
  }

  impl_->running.store(true);
  impl_->t = std::thread([impl = impl_] {
    impl->http.listen_after_bind();
    impl->running.store(false);
  });

  std::fprintf(stderr, "khor-daemon: listening on http://%s:%d\n", host.c_str(), port);
  return true;
}

void HttpServer::stop() {
  if (!impl_) return;
  if (impl_->running.exchange(false)) {
    impl_->http.stop();
  }
  if (impl_->t.joinable()) impl_->t.join();
}

} // namespace khor

