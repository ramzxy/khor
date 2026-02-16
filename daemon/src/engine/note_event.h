#pragma once

namespace khor {

struct NoteEvent {
  int midi = 60;         // 0..127
  float velocity = 0.7f; // 0..1
  float dur_s = 0.25f;   // >0
};

} // namespace khor

