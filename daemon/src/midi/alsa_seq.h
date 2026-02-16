#pragma once

#include <cstdint>
#include <string>

#include "engine/note_event.h"
#include "engine/signals.h"

namespace khor {

struct MidiStatus {
  bool enabled = false;
  bool ok = false;
  std::string port;
  int channel = 1;
  std::string error;
};

class MidiOut {
 public:
  MidiOut();
  ~MidiOut();

  MidiOut(const MidiOut&) = delete;
  MidiOut& operator=(const MidiOut&) = delete;

  bool start(const std::string& port_name, int channel_1_16, std::string* err);
  void stop();
  bool is_running() const;

  void send_note(const NoteEvent& ev);
  void send_signals_cc(const Signal01& s, float cutoff01);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace khor

