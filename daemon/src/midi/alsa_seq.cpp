#include "midi/alsa_seq.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(KHOR_HAS_ALSA_SEQ)
#include <alsa/asoundlib.h>
#endif

namespace khor {

struct MidiOut::Impl {
#if defined(KHOR_HAS_ALSA_SEQ)
  snd_seq_t* seq = nullptr;
  int port = -1;
  std::string port_name;

  std::atomic<bool> running{false};
  std::thread worker;

  struct NoteKey {
    int channel;
    int midi;
    bool operator==(const NoteKey& o) const { return channel == o.channel && midi == o.midi; }
  };

  struct PendingOff {
    std::chrono::steady_clock::time_point due;
    NoteKey key;
  };
  std::mutex mu;
  std::vector<PendingOff> offs;

  std::chrono::steady_clock::time_point last_cc = std::chrono::steady_clock::time_point{};

  static int vel_0_127(float v01) {
    v01 = std::clamp(v01, 0.0f, 1.0f);
    return (int)std::lround(v01 * 127.0f);
  }

  void send_event(const snd_seq_event_t* ev) {
    if (!seq || !ev) return;
    (void)snd_seq_event_output_direct(seq, const_cast<snd_seq_event_t*>(ev));
  }

  void send_note_on(int ch, int midi, int vel) {
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteon(&ev, ch, midi, vel);
    send_event(&ev);
  }

  void send_note_off(int ch, int midi) {
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteoff(&ev, ch, midi, 0);
    send_event(&ev);
  }

  void send_cc(int ch, int cc, int value) {
    cc = std::clamp(cc, 0, 127);
    value = std::clamp(value, 0, 127);
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_controller(&ev, ch, cc, value);
    send_event(&ev);
  }

  void loop() {
    while (running.load(std::memory_order_relaxed)) {
      std::vector<NoteKey> due_notes;
      auto now = std::chrono::steady_clock::now();

      {
        std::scoped_lock lk(mu);
        auto it = offs.begin();
        while (it != offs.end()) {
          if (it->due <= now) {
            due_notes.push_back(it->key);
            it = offs.erase(it);
          } else {
            ++it;
          }
        }
      }

      for (const auto& k : due_notes) send_note_off(k.channel, k.midi);

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
#else
  std::string why = "built without ALSA sequencer support (install alsa-lib-devel and rebuild)";
#endif
};

MidiOut::MidiOut() : impl_(new Impl()) {}
MidiOut::~MidiOut() { stop(); delete impl_; impl_ = nullptr; }

bool MidiOut::start(const std::string& port_name, int channel_1_16, std::string* err) {
  if (!impl_) return false;
#if !defined(KHOR_HAS_ALSA_SEQ)
  if (err) *err = impl_->why;
  (void)port_name;
  (void)channel_1_16;
  return false;
#else
  stop();

  (void)channel_1_16; // channel now per-note, not global
  impl_->port_name = port_name.empty() ? "khor" : port_name;

  if (snd_seq_open(&impl_->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0 || !impl_->seq) {
    if (err) *err = "snd_seq_open failed";
    stop();
    return false;
  }
  snd_seq_set_client_name(impl_->seq, "khor");

  impl_->port = snd_seq_create_simple_port(
    impl_->seq,
    impl_->port_name.c_str(),
    SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
    SND_SEQ_PORT_TYPE_APPLICATION
  );
  if (impl_->port < 0) {
    if (err) *err = "snd_seq_create_simple_port failed";
    stop();
    return false;
  }

  impl_->running.store(true, std::memory_order_relaxed);
  impl_->worker = std::thread([impl = impl_] { impl->loop(); });
  return true;
#endif
}

void MidiOut::stop() {
  if (!impl_) return;
#if defined(KHOR_HAS_ALSA_SEQ)
  impl_->running.store(false, std::memory_order_relaxed);
  if (impl_->worker.joinable()) impl_->worker.join();
  if (impl_->seq) snd_seq_close(impl_->seq);
  impl_->seq = nullptr;
  impl_->port = -1;
  impl_->offs.clear();
#endif
}

bool MidiOut::is_running() const {
  if (!impl_) return false;
#if defined(KHOR_HAS_ALSA_SEQ)
  return impl_->seq != nullptr && impl_->port >= 0;
#else
  return false;
#endif
}

void MidiOut::send_note(const NoteEvent& ev) {
  if (!impl_ || !is_running()) return;
#if defined(KHOR_HAS_ALSA_SEQ)
  const int ch = std::clamp(ev.channel, 1, 16) - 1; // MIDI 0-indexed
  const int midi = std::clamp(ev.midi, 0, 127);
  const int vel = Impl::vel_0_127(ev.velocity);
  impl_->send_note_on(ch, midi, vel);

  const float dur = std::max(0.02f, ev.dur_s);
  const auto due = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(dur));
  {
    std::scoped_lock lk(impl_->mu);
    impl_->offs.push_back(Impl::PendingOff{.due = due, .key = {.channel = ch, .midi = midi}});
  }
#else
  (void)ev;
#endif
}

void MidiOut::send_signals_cc(const Signal01& s, float cutoff01) {
  if (!impl_ || !is_running()) return;
#if defined(KHOR_HAS_ALSA_SEQ)
  auto now = std::chrono::steady_clock::now();
  if (impl_->last_cc.time_since_epoch().count() != 0) {
    auto dt = now - impl_->last_cc;
    if (dt < std::chrono::milliseconds(80)) return;
  }
  impl_->last_cc = now;

  impl_->send_cc(0, 20, Impl::vel_0_127((float)s.exec));
  impl_->send_cc(0, 21, Impl::vel_0_127((float)s.rx));
  impl_->send_cc(0, 22, Impl::vel_0_127((float)s.tx));
  impl_->send_cc(0, 23, Impl::vel_0_127((float)s.csw));
  impl_->send_cc(0, 24, Impl::vel_0_127((float)s.io));
  impl_->send_cc(0, 74, Impl::vel_0_127(cutoff01));
#else
  (void)s;
  (void)cutoff01;
#endif
}

} // namespace khor

