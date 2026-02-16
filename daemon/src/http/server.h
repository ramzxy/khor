#pragma once

#include <string>

namespace khor {

class App;

class HttpServer {
 public:
  explicit HttpServer(App* app);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool start(const std::string& host, int port, const std::string& ui_dir, bool serve_ui, std::string* err);
  void stop();
  bool is_running() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace khor

