#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>

#include "engine/note_event.h"
#include "engine/signals.h"

namespace khor::osc {

inline void pad4(std::vector<uint8_t>& b) {
  while ((b.size() & 3u) != 0) b.push_back(0);
}

inline void put_str(std::vector<uint8_t>& b, const char* s) {
  if (!s) s = "";
  const std::size_t n = std::strlen(s);
  b.insert(b.end(), (const uint8_t*)s, (const uint8_t*)s + n);
  b.push_back(0);
  pad4(b);
}

inline void put_i32(std::vector<uint8_t>& b, int32_t v) {
  uint32_t be = htonl((uint32_t)v);
  const uint8_t* p = (const uint8_t*)&be;
  b.insert(b.end(), p, p + 4);
}

inline void put_f32(std::vector<uint8_t>& b, float f) {
  uint32_t u = 0;
  static_assert(sizeof(float) == sizeof(uint32_t));
  std::memcpy(&u, &f, sizeof(u));
  u = htonl(u);
  const uint8_t* p = (const uint8_t*)&u;
  b.insert(b.end(), p, p + 4);
}

inline std::vector<uint8_t> encode_note(const NoteEvent& ev) {
  std::vector<uint8_t> b;
  b.reserve(64);
  put_str(b, "/khor/note");
  put_str(b, ",iff");
  put_i32(b, (int32_t)std::clamp(ev.midi, 0, 127));
  put_f32(b, std::clamp(ev.velocity, 0.0f, 1.0f));
  put_f32(b, std::max(0.0f, ev.dur_s));
  return b;
}

inline std::vector<uint8_t> encode_signal(const char* name, float v01) {
  std::vector<uint8_t> b;
  b.reserve(96);
  put_str(b, "/khor/signal");
  put_str(b, ",sf");
  put_str(b, name ? name : "");
  put_f32(b, std::clamp(v01, 0.0f, 1.0f));
  return b;
}

inline std::vector<uint8_t> encode_metrics(const SignalRates& r) {
  std::vector<uint8_t> b;
  b.reserve(128);
  put_str(b, "/khor/metrics");
  put_str(b, ",ffffff");
  put_f32(b, (float)r.exec_s);
  put_f32(b, (float)r.rx_kbs);
  put_f32(b, (float)r.tx_kbs);
  put_f32(b, (float)r.csw_s);
  put_f32(b, (float)r.blk_r_kbs);
  put_f32(b, (float)r.blk_w_kbs);
  return b;
}

} // namespace khor::osc

