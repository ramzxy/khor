#pragma once

#include <cstdint>
#include <string>

#include "engine/note_event.h"
#include "engine/signals.h"

namespace khor {

struct OscStatus {
  bool enabled = false;
  bool ok = false;
  std::string host;
  int port = 0;
  std::string error;
};

class OscClient {
 public:
  OscClient();
  ~OscClient();

  OscClient(const OscClient&) = delete;
  OscClient& operator=(const OscClient&) = delete;

  bool start(const std::string& host, int port, std::string* err);
  void stop();
  bool is_running() const;

  void send_note(const NoteEvent& ev);
  void send_signal(const char* name, float value01);
  void send_metrics(const SignalRates& r);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace khor

