#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "engine/music.h"
#include "engine/signals.h"
#include "osc/encode.h"

namespace {

static int g_fail = 0;

struct Test {
  const char* name;
  void (*fn)();
};

static std::vector<Test>& tests() {
  static std::vector<Test> t;
  return t;
}

struct Reg {
  Reg(const char* name, void (*fn)()) { tests().push_back(Test{name, fn}); }
};

#define TEST_CASE(name) \
  static void test_##name(); \
  static Reg reg_##name(#name, &test_##name); \
  static void test_##name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_fail++; \
    } \
  } while (0)

static bool approx(double a, double b, double eps) {
  return std::abs(a - b) <= eps;
}

TEST_CASE(signals_rates_and_smoothing) {
  khor::Signals s;
  khor::Signals::Totals t0{};
  khor::Signals::Totals t1{};
  t1.exec_total = 100;
  t1.net_rx_bytes_total = 1024 * 10;

  s.update(t0, 1.0, 0.0);
  s.update(t1, 1.0, 0.0);

  const auto r = s.rates();
  CHECK(approx(r.exec_s, 100.0, 1e-6));
  CHECK(approx(r.rx_kbs, 10.0, 1e-6));

  const auto v = s.value01();
  CHECK(v.exec > 0.0 && v.exec <= 1.0);
  CHECK(v.rx > 0.0 && v.rx <= 1.0);
}

TEST_CASE(music_silence_vs_drone) {
  khor::MusicEngine eng;
  khor::Signal01 z{};

  khor::MusicConfig ambient;
  ambient.preset = "ambient";
  ambient.scale = "pentatonic_minor";
  ambient.key_midi = 62;
  ambient.density = 0.5;

  auto a = eng.tick(z, ambient);
  CHECK(a.notes.empty());

  khor::MusicEngine eng2;
  khor::MusicConfig drone = ambient;
  drone.preset = "drone";
  auto d = eng2.tick(z, drone);
  CHECK(!d.notes.empty());
  for (const auto& n : d.notes) CHECK(n.midi >= 0 && n.midi <= 127);
}

TEST_CASE(adsr_envelope) {
  khor::dsp::Adsr e;
  e.a_s = 0.01f;
  e.d_s = 0.01f;
  e.s_level = 0.5f;
  e.r_s = 0.02f;
  const float sr = 1000.0f;

  e.note_on(sr);
  float peak = 0.0f;
  for (int i = 0; i < 40; i++) peak = std::max(peak, e.tick(sr));
  CHECK(peak > 0.95f);

  // Let it settle near sustain.
  for (int i = 0; i < 50; i++) e.tick(sr);
  CHECK(std::abs(e.value - 0.5f) < 0.08f);

  e.note_off(sr);
  for (int i = 0; i < 80; i++) e.tick(sr);
  CHECK(e.stage == khor::dsp::Adsr::Off);
  CHECK(e.value <= 1e-6f);
}

static std::string osc_read_str(const std::vector<uint8_t>& b, std::size_t* off) {
  std::size_t i = off ? *off : 0;
  std::string s;
  while (i < b.size() && b[i] != 0) s.push_back((char)b[i++]);
  // skip null
  if (i < b.size()) i++;
  // pad to 4
  while ((i & 3u) != 0 && i < b.size()) i++;
  if (off) *off = i;
  return s;
}

static uint32_t osc_read_u32(const std::vector<uint8_t>& b, std::size_t* off) {
  std::size_t i = off ? *off : 0;
  if (i + 4 > b.size()) return 0;
  uint32_t u = 0;
  std::memcpy(&u, &b[i], 4);
  u = ntohl(u);
  if (off) *off = i + 4;
  return u;
}

TEST_CASE(osc_encoding_note) {
  khor::NoteEvent ev;
  ev.midi = 64;
  ev.velocity = 0.5f;
  ev.dur_s = 0.25f;

  const auto msg = khor::osc::encode_note(ev);
  CHECK((msg.size() & 3u) == 0u);

  std::size_t off = 0;
  const std::string addr = osc_read_str(msg, &off);
  const std::string tt = osc_read_str(msg, &off);
  CHECK(addr == "/khor/note");
  CHECK(tt == ",iff");

  const uint32_t midi = osc_read_u32(msg, &off);
  CHECK(midi == 64u);
}

} // namespace

int main() {
  for (const auto& t : tests()) {
    std::fprintf(stderr, "TEST %s\n", t.name);
    t.fn();
  }
  if (g_fail) {
    std::fprintf(stderr, "FAILURES: %d\n", g_fail);
    return 1;
  }
  std::fprintf(stderr, "OK (%zu tests)\n", tests().size());
  return 0;
}
