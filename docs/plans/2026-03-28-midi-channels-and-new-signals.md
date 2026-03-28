# Multi-Channel MIDI + New Kernel Signals Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make khor's MIDI output DAW-grade for live multi-instrument use, and add three new kernel signals (memory pressure, TCP retransmits, IRQ rate) with distinct temporal characters for richer sonification.

**Architecture:** Two independent features. Feature 1 adds a `channel` field to `NoteEvent`, assigns channels per voice role in each music preset, fixes MIDI note-off collisions, and passes channel through MIDI/OSC outputs. Feature 2 adds new counters to the eBPF layer (TCP retransmits, IRQ rate), reads PSI memory pressure from procfs, normalizes all three in the signal pipeline, and maps them musically.

**Tech Stack:** C++20, eBPF/libbpf, ALSA sequencer, miniaudio, OSC over UDP

**Build:** `./scripts/linux-build.sh` (cmake + make)
**Test:** `./daemon/build/khor-tests`

---

## Feature 1: Multi-Channel MIDI

### Task 1: Add `channel` field to NoteEvent

**Files:**
- Modify: `daemon/src/engine/note_event.h:5-9`

**Step 1: Add channel field**

In `daemon/src/engine/note_event.h`, add `channel` after `dur_s`:

```cpp
struct NoteEvent {
  int midi = 60;         // 0..127
  float velocity = 0.7f; // 0..1
  float dur_s = 0.25f;   // >0
  int channel = 1;       // 1..16 (MIDI convention)
};
```

**Step 2: Build to verify no breakage**

Run: `cd /home/ilia/coding/khor && ./scripts/linux-build.sh`
Expected: Clean build. The new field defaults to 1, so all existing code continues to work.

**Step 3: Run tests**

Run: `./daemon/build/khor-tests`
Expected: All existing tests pass.

**Step 4: Commit**

```bash
git add daemon/src/engine/note_event.h
git commit -m "feat: add channel field to NoteEvent for multi-channel MIDI"
```

---

### Task 2: Assign channels per voice role in music engine

**Files:**
- Modify: `daemon/src/engine/music.cpp:104-202` (preset blocks)

**Step 1: Write a test for channel assignment**

In `daemon/tests/test_main.cpp`, add after the `music_silence_vs_drone` test:

```cpp
TEST_CASE(music_channel_assignment) {
  // Drone preset should produce bass notes on channel 2.
  khor::MusicEngine eng;
  khor::Signal01 active{};
  active.exec = 0.5;
  active.rx = 0.3;
  active.io = 0.4;

  khor::MusicConfig drone;
  drone.preset = "drone";
  drone.key_midi = 62;
  drone.density = 0.5;
  drone.scale = "pentatonic_minor";

  auto frame = eng.tick(active, drone);
  // Step 0 of drone always emits the low root on channel 2.
  CHECK(!frame.notes.empty());
  bool has_bass = false;
  for (const auto& n : frame.notes) {
    CHECK(n.channel >= 1 && n.channel <= 16);
    if (n.channel == 2) has_bass = true;
  }
  CHECK(has_bass);
}
```

**Step 2: Run test to verify it fails**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: FAIL — channel is 1 (default) for all notes, `has_bass` is false.

**Step 3: Assign channels in all four presets**

In `daemon/src/engine/music.cpp`, define channel constants near the top of the file (inside the anonymous namespace, after `push_note`):

```cpp
// MIDI channel convention for DAW routing.
static constexpr int kChMelody = 1;
static constexpr int kChBass   = 2;
static constexpr int kChChords = 3;
static constexpr int kChPerc   = 10;
```

Then in each preset, set `channel` on notes before pushing. The changes are:

**Ambient preset** (around line 108-125):
- Melody note (the `p_note` block): after `push_note(out.notes, midi, vel, dur);` — set `out.notes.back().channel = kChMelody;`
- Exec accent dyads (the `p_exec` block): after both `push_note` calls — set `out.notes[out.notes.size()-1].channel = kChChords;` and `out.notes[out.notes.size()-2].channel = kChChords;`

**Percussive preset** (around line 126-154):
- Kick (the `p_kick` block): `out.notes.back().channel = kChBass;`
- Click (the `p_click` block): `out.notes.back().channel = kChPerc;`
- Mid hit (the `p_mid` block): `out.notes.back().channel = kChPerc;`

**Arp preset** (around line 155-178):
- Arp note (the `p_arp` block): `out.notes.back().channel = kChMelody;`
- Chord stab root+up (the `p_stab` block): both `out.notes.back().channel = kChChords;` (and the one before)

**Drone preset** (around line 179-202):
- Low root (step 0): `out.notes.back().channel = kChBass;`
- Mid note (step 8): `out.notes.back().channel = kChBass;`
- Network sprinkle (the `p_top` block): `out.notes.back().channel = kChMelody;`

A cleaner approach: modify `push_note` to accept channel:

```cpp
static void push_note(std::vector<NoteEvent>& out, int midi, float vel, float dur_s, int ch = 1) {
  NoteEvent ev;
  ev.midi = std::clamp(midi, 0, 127);
  ev.velocity = clamp01f(vel);
  ev.dur_s = std::max(0.02f, dur_s);
  ev.channel = ch;
  out.push_back(ev);
}
```

Then pass the channel as the last argument to every `push_note` call:

- Ambient melody: `push_note(out.notes, midi, vel, dur, kChMelody);`
- Ambient exec root: `push_note(out.notes, root, 0.42f, 0.35f, kChChords);`
- Ambient exec fifth: `push_note(out.notes, fifth, 0.30f, 0.35f, kChChords);`
- Percussive kick: `push_note(out.notes, midi, ..., 0.08f, kChBass);`
- Percussive click: `push_note(out.notes, midi, ..., 0.05f, kChPerc);`
- Percussive mid: `push_note(out.notes, midi, ..., 0.07f, kChPerc);`
- Arp note: `push_note(out.notes, midi, vel, 0.12f, kChMelody);`
- Arp stab root: `push_note(out.notes, root, 0.45f, 0.20f, kChChords);`
- Arp stab up: `push_note(out.notes, up, 0.30f, 0.20f, kChChords);`
- Drone low root: `push_note(out.notes, midi, ..., 2.3f, kChBass);`
- Drone mid: `push_note(out.notes, midi, ..., 1.6f, kChBass);`
- Drone sprinkle: `push_note(out.notes, midi, ..., 0.40f, kChMelody);`

**Step 4: Run tests**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass including the new `music_channel_assignment`.

**Step 5: Commit**

```bash
git add daemon/src/engine/music.cpp daemon/tests/test_main.cpp
git commit -m "feat: assign MIDI channels per voice role in all presets"
```

---

### Task 3: Fix MIDI note-off collisions

**Files:**
- Modify: `daemon/src/midi/alsa_seq.cpp:17-104` (Impl struct)

**Step 1: Replace note-off tracking with (channel, midi) keyed counters**

In `daemon/src/midi/alsa_seq.cpp`, inside the `Impl` struct under `#if defined(KHOR_HAS_ALSA_SEQ)`:

Replace the `PendingOff` struct and `offs` vector (lines 27-32):

```cpp
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
```

Update `send_note_on` and `send_note_off` to accept channel:

```cpp
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
```

Update the `loop()` method to use `NoteKey`:

```cpp
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
```

**Step 2: Update `send_note()` to use ev.channel**

In the `MidiOut::send_note` function (around line 169-185):

```cpp
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
```

**Step 3: Update `send_signals_cc` to use channel 0 (MIDI ch 1)**

In `MidiOut::send_signals_cc` (around line 187-207), replace `impl_->send_cc(20, ...)` calls — `send_cc` should also accept channel. Update it:

```cpp
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
```

And update the calls in `send_signals_cc`:

```cpp
  impl_->send_cc(0, 20, Impl::vel_0_127((float)s.exec));
  impl_->send_cc(0, 21, Impl::vel_0_127((float)s.rx));
  impl_->send_cc(0, 22, Impl::vel_0_127((float)s.tx));
  impl_->send_cc(0, 23, Impl::vel_0_127((float)s.csw));
  impl_->send_cc(0, 24, Impl::vel_0_127((float)s.io));
  impl_->send_cc(0, 74, Impl::vel_0_127(cutoff01));
```

**Step 4: Build and test**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass. (MIDI code is not directly unit-tested since it requires ALSA, but compilation must succeed.)

**Step 5: Commit**

```bash
git add daemon/src/midi/alsa_seq.cpp
git commit -m "fix: track MIDI note-offs by (channel, midi) pair to prevent collisions"
```

---

### Task 4: Update OSC output to include channel

**Files:**
- Modify: `daemon/src/osc/encode.h:43-52` (encode_note function)

**Step 1: Update the test for OSC encoding**

In `daemon/tests/test_main.cpp`, update the existing `osc_encoding_note` test:

```cpp
TEST_CASE(osc_encoding_note) {
  khor::NoteEvent ev;
  ev.midi = 64;
  ev.velocity = 0.5f;
  ev.dur_s = 0.25f;
  ev.channel = 10;

  const auto msg = khor::osc::encode_note(ev);
  CHECK((msg.size() & 3u) == 0u);

  std::size_t off = 0;
  const std::string addr = osc_read_str(msg, &off);
  const std::string tt = osc_read_str(msg, &off);
  CHECK(addr == "/khor/note");
  CHECK(tt == ",iiff");

  const uint32_t ch = osc_read_u32(msg, &off);
  CHECK(ch == 10u);
  const uint32_t midi = osc_read_u32(msg, &off);
  CHECK(midi == 64u);
}
```

**Step 2: Run test to verify it fails**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: FAIL — type tag is still `,iff` and channel is not encoded.

**Step 3: Update encode_note to include channel**

In `daemon/src/osc/encode.h`, update `encode_note`:

```cpp
inline std::vector<uint8_t> encode_note(const NoteEvent& ev) {
  std::vector<uint8_t> b;
  b.reserve(64);
  put_str(b, "/khor/note");
  put_str(b, ",iiff");
  put_i32(b, (int32_t)std::clamp(ev.channel, 1, 16));
  put_i32(b, (int32_t)std::clamp(ev.midi, 0, 127));
  put_f32(b, std::clamp(ev.velocity, 0.0f, 1.0f));
  put_f32(b, std::max(0.0f, ev.dur_s));
  return b;
}
```

**Step 4: Run tests**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add daemon/src/osc/encode.h daemon/tests/test_main.cpp
git commit -m "feat: include channel in OSC note messages"
```

---

## Feature 2: New Kernel Signals

### Task 5: Add new counters to KhorMetrics and shared BPF header

**Files:**
- Modify: `daemon/include/khor/metrics.h:6-19`
- Modify: `bpf/khor.h:23-28, 38-47`

**Step 1: Add new fields to KhorMetrics**

In `daemon/include/khor/metrics.h`:

```cpp
struct KhorMetrics {
  std::atomic<uint64_t> events_total{0};
  std::atomic<uint64_t> events_dropped{0};

  std::atomic<uint64_t> exec_total{0};
  std::atomic<uint64_t> net_rx_bytes_total{0};
  std::atomic<uint64_t> net_tx_bytes_total{0};
  std::atomic<uint64_t> sched_switch_total{0};
  std::atomic<uint64_t> blk_read_bytes_total{0};
  std::atomic<uint64_t> blk_write_bytes_total{0};

  // New signals.
  std::atomic<uint64_t> tcp_retransmit_total{0};
  std::atomic<uint64_t> irq_total{0};
  std::atomic<double> mem_pressure_pct{0.0}; // PSI some avg10, 0..100

  std::atomic<double> bpm{110.0};
  std::atomic<int> key_midi{62}; // D4
};
```

**Step 2: Add new probe masks and payload fields to khor.h**

In `bpf/khor.h`, add to the `khor_probe_mask` enum:

```cpp
enum khor_probe_mask {
  KHOR_PROBE_EXEC  = 1u << 0,
  KHOR_PROBE_NET   = 1u << 1,
  KHOR_PROBE_SCHED = 1u << 2,
  KHOR_PROBE_BLOCK = 1u << 3,
  KHOR_PROBE_TCP   = 1u << 4,
  KHOR_PROBE_IRQ   = 1u << 5,
};
```

In `khor_sample_payload`, add the new fields (this struct has room in the `_u64[8]` union):

```cpp
struct khor_sample_payload {
  khor_u64 exec_count;
  khor_u64 net_rx_bytes;
  khor_u64 net_tx_bytes;
  khor_u64 sched_switches;
  khor_u64 blk_read_bytes;
  khor_u64 blk_write_bytes;
  khor_u64 blk_issue_count;
  khor_u64 lost_events;
  khor_u64 tcp_retransmits;
  khor_u64 irq_count;
};
```

Also grow the union padding to accommodate:

```cpp
  union {
    struct khor_sample_payload sample;
    khor_u64 _u64[10]; // keep event size stable
  } u;
```

**Step 3: Build to verify**

Run: `./scripts/linux-build.sh`
Expected: Clean build. New fields default to 0.

**Step 4: Commit**

```bash
git add daemon/include/khor/metrics.h bpf/khor.h
git commit -m "feat: add TCP retransmit, IRQ, and memory pressure counters"
```

---

### Task 6: Add new eBPF tracepoints

**Files:**
- Modify: `bpf/khor.bpf.c` (add two new SEC programs + reset new fields in maybe_flush)

**Step 1: Add TCP retransmit tracepoint**

At the end of `bpf/khor.bpf.c`, before the closing, add:

```c
SEC("tracepoint/tcp/tcp_retransmit_skb")
int tp_tcp_retransmit(struct trace_event_raw_tcp_retransmit_skb* ctx) {
  (void)ctx;
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_TCP)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.tcp_retransmits++;
  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}
```

**Step 2: Add IRQ handler entry tracepoint**

```c
SEC("tracepoint/irq/irq_handler_entry")
int tp_irq_entry(struct trace_event_raw_irq_handler_entry* ctx) {
  (void)ctx;
  const struct khor_bpf_config* cfg = get_cfg();
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_IRQ)) return 0;
  // Skip pass_filters for IRQs — they aren't process-scoped.

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.irq_count++;
  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}
```

**Step 3: Update `cfg_enabled_mask` default to include new probes**

In the `cfg_enabled_mask` function (line 41-45), update `all`:

```c
static __always_inline __u32 cfg_enabled_mask(const struct khor_bpf_config* cfg) {
  const __u32 all = (KHOR_PROBE_EXEC | KHOR_PROBE_NET | KHOR_PROBE_SCHED | KHOR_PROBE_BLOCK | KHOR_PROBE_TCP | KHOR_PROBE_IRQ);
  if (!cfg) return all;
  return cfg->enabled_mask ? cfg->enabled_mask : all;
}
```

**Step 4: Update `maybe_flush` to reset new counters**

In `maybe_flush` (around line 96-119), add after `c->acc.blk_issue_count = 0;`:

```c
  c->acc.tcp_retransmits = 0;
  c->acc.irq_count = 0;
```

And update the "anything to flush?" check to include the new fields:

```c
  if (c->acc.exec_count || c->acc.net_rx_bytes || c->acc.net_tx_bytes || c->acc.sched_switches ||
      c->acc.blk_read_bytes || c->acc.blk_write_bytes || c->acc.blk_issue_count ||
      c->acc.tcp_retransmits || c->acc.irq_count || c->acc.lost_events) {
```

**Step 5: Build**

Run: `./scripts/linux-build.sh`
Expected: Clean build. Note: the BPF tracepoint struct names (e.g. `trace_event_raw_tcp_retransmit_skb`) come from `vmlinux.h` generated by bpftool. If the kernel doesn't have a tracepoint, the BPF compile step will fail — that's expected on kernels without these tracepoints. The daemon still compiles without BPF support.

**Step 6: Commit**

```bash
git add bpf/khor.bpf.c
git commit -m "feat: add eBPF tracepoints for TCP retransmits and IRQ rate"
```

---

### Task 7: Consume new BPF payload fields in collector

**Files:**
- Modify: `daemon/src/bpf/collector.cpp:160-173` (on_event lambda)

**Step 1: Update the ring buffer callback**

In `daemon/src/bpf/collector.cpp`, in the `on_event` lambda (around line 160-174), add after the `blk_write_bytes_total` line:

```cpp
      m->tcp_retransmit_total.fetch_add(e->u.sample.tcp_retransmits, std::memory_order_relaxed);
      m->irq_total.fetch_add(e->u.sample.irq_count, std::memory_order_relaxed);
```

**Step 2: Build**

Run: `./scripts/linux-build.sh`
Expected: Clean build.

**Step 3: Commit**

```bash
git add daemon/src/bpf/collector.cpp
git commit -m "feat: consume TCP retransmit and IRQ counters from BPF ring buffer"
```

---

### Task 8: Add new signals to Signal01 and normalization

**Files:**
- Modify: `daemon/src/engine/signals.h:7-14, 16-23, 28-35`
- Modify: `daemon/src/engine/signals.cpp:29-63`

**Step 1: Write test for new signal normalization**

In `daemon/tests/test_main.cpp`, add:

```cpp
TEST_CASE(signals_new_counters) {
  khor::Signals s;
  khor::Signals::Totals t0{};
  khor::Signals::Totals t1{};
  t1.tcp_retransmit_total = 5;
  t1.irq_total = 10000;

  s.update(t0, 1.0, 0.0);
  s.update(t1, 1.0, 0.0, 25.0); // 25% memory pressure

  const auto r = s.rates();
  CHECK(approx(r.retx_s, 5.0, 1e-6));
  CHECK(approx(r.irq_s, 10000.0, 1e-6));
  CHECK(approx(r.mem_pct, 25.0, 1e-6));

  const auto v = s.value01();
  CHECK(v.retx > 0.0 && v.retx <= 1.0);
  CHECK(v.irq > 0.0 && v.irq <= 1.0);
  CHECK(v.mem > 0.0 && v.mem <= 1.0);
}
```

**Step 2: Run to verify it fails**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: Compile error — `Totals` doesn't have the new fields yet.

**Step 3: Update SignalRates**

In `daemon/src/engine/signals.h`, add to `SignalRates`:

```cpp
struct SignalRates {
  double exec_s = 0.0;
  double rx_kbs = 0.0;
  double tx_kbs = 0.0;
  double csw_s = 0.0;
  double blk_r_kbs = 0.0;
  double blk_w_kbs = 0.0;
  double retx_s = 0.0;   // TCP retransmits/sec
  double irq_s = 0.0;    // IRQs/sec
  double mem_pct = 0.0;   // memory pressure % (0..100)
};
```

**Step 4: Update Signal01**

```cpp
struct Signal01 {
  double exec = 0.0;
  double rx = 0.0;
  double tx = 0.0;
  double csw = 0.0;
  double io = 0.0;
  double retx = 0.0;  // TCP retransmits (spiky)
  double irq = 0.0;   // IRQ rate (fast texture)
  double mem = 0.0;    // memory pressure (slow mood)
};
```

**Step 5: Update Totals**

```cpp
  struct Totals {
    uint64_t exec_total = 0;
    uint64_t net_rx_bytes_total = 0;
    uint64_t net_tx_bytes_total = 0;
    uint64_t sched_switch_total = 0;
    uint64_t blk_read_bytes_total = 0;
    uint64_t blk_write_bytes_total = 0;
    uint64_t tcp_retransmit_total = 0;
    uint64_t irq_total = 0;
  };
```

**Step 6: Update `Signals::update` signature and implementation**

Change the signature to accept `mem_pressure_pct`:

In `daemon/src/engine/signals.h`:
```cpp
  void update(const Totals& cur, double dt_s, double smoothing01, double mem_pressure_pct = 0.0);
```

In `daemon/src/engine/signals.cpp`, update the implementation:

```cpp
void Signals::update(const Totals& cur, double dt_s, double smoothing01, double mem_pressure_pct) {
  cur_ = cur;
  if (!has_prev_) {
    prev_ = cur;
    has_prev_ = true;
    rates_ = SignalRates{};
    v01_ = Signal01{};
    return;
  }

  if (dt_s <= 0.0) dt_s = 0.1;

  rates_.exec_s = (double)(cur.exec_total - prev_.exec_total) / dt_s;
  rates_.rx_kbs = (double)(cur.net_rx_bytes_total - prev_.net_rx_bytes_total) / dt_s / 1024.0;
  rates_.tx_kbs = (double)(cur.net_tx_bytes_total - prev_.net_tx_bytes_total) / dt_s / 1024.0;
  rates_.csw_s = (double)(cur.sched_switch_total - prev_.sched_switch_total) / dt_s;
  rates_.blk_r_kbs = (double)(cur.blk_read_bytes_total - prev_.blk_read_bytes_total) / dt_s / 1024.0;
  rates_.blk_w_kbs = (double)(cur.blk_write_bytes_total - prev_.blk_write_bytes_total) / dt_s / 1024.0;
  rates_.retx_s = (double)(cur.tcp_retransmit_total - prev_.tcp_retransmit_total) / dt_s;
  rates_.irq_s = (double)(cur.irq_total - prev_.irq_total) / dt_s;
  rates_.mem_pct = mem_pressure_pct;

  const double exec01 = norm_log(rates_.exec_s, 250.0);
  const double rx01 = norm_log(rates_.rx_kbs, 50000.0);
  const double tx01 = norm_log(rates_.tx_kbs, 50000.0);
  const double csw01 = norm_log(rates_.csw_s, 120000.0);
  const double io01 = norm_log(rates_.blk_r_kbs + rates_.blk_w_kbs, 80000.0);
  const double retx01 = norm_log(rates_.retx_s, 50.0);     // 50 retx/sec is severe
  const double irq01 = norm_log(rates_.irq_s, 200000.0);   // 200k IRQs/sec is busy
  const double mem01 = clamp01(mem_pressure_pct / 100.0);   // already 0-100, just scale

  v01_.exec = ema(v01_.exec, exec01, smoothing01);
  v01_.rx = ema(v01_.rx, rx01, smoothing01);
  v01_.tx = ema(v01_.tx, tx01, smoothing01);
  v01_.csw = ema(v01_.csw, csw01, smoothing01);
  v01_.io = ema(v01_.io, io01, smoothing01);
  v01_.retx = ema(v01_.retx, retx01, smoothing01 * 0.5); // less smoothing for spiky signal
  v01_.irq = ema(v01_.irq, irq01, smoothing01);
  v01_.mem = ema(v01_.mem, mem01, 0.95);                  // very smooth, slow-moving

  prev_ = cur;
}
```

Note: `retx` uses half the smoothing factor so spikes come through faster (it's meant to be a percussive trigger). `mem` uses fixed high smoothing (0.95) since PSI already provides a 10-second average.

**Step 7: Run tests**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass including the new `signals_new_counters`.

**Step 8: Commit**

```bash
git add daemon/src/engine/signals.h daemon/src/engine/signals.cpp daemon/tests/test_main.cpp
git commit -m "feat: normalize TCP retransmit, IRQ, and memory pressure signals"
```

---

### Task 9: Read PSI memory pressure in sampler_loop + wire new Totals

**Files:**
- Modify: `daemon/src/app/app.cpp:220-257` (sampler_loop)
- Modify: `daemon/src/app/app.cpp:326-336` (fake_loop)

**Step 1: Add PSI reader helper**

At the top of `daemon/src/app/app.cpp`, inside the anonymous namespace (after `json_error`), add:

```cpp
static double read_psi_memory_some_avg10() {
  // /proc/pressure/memory contains lines like:
  //   some avg10=0.00 avg60=0.00 avg300=0.00 total=12345
  //   full avg10=0.00 avg60=0.00 avg300=0.00 total=6789
  FILE* f = std::fopen("/proc/pressure/memory", "r");
  if (!f) return 0.0;
  char line[256];
  double avg10 = 0.0;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "some ", 5) == 0) {
      // Parse "some avg10=X.XX"
      const char* p = std::strstr(line, "avg10=");
      if (p) avg10 = std::strtod(p + 6, nullptr);
      break;
    }
  }
  std::fclose(f);
  return std::clamp(avg10, 0.0, 100.0);
}
```

**Step 2: Update sampler_loop to read new counters and PSI**

In `sampler_loop` (around line 220-257), update the Totals reading and the `signals_.update` call:

After the existing `t.blk_write_bytes_total` line, add:
```cpp
    t.tcp_retransmit_total = metrics_.tcp_retransmit_total.load(std::memory_order_relaxed);
    t.irq_total = metrics_.irq_total.load(std::memory_order_relaxed);
```

For PSI, read it every ~1s (every 10th tick since sampler runs at 100ms). Add a counter before the while loop:

```cpp
  int psi_tick = 0;
  double mem_psi = 0.0;
```

Inside the loop, before the `signals_.update` call:
```cpp
    if (++psi_tick >= 10) {
      psi_tick = 0;
      mem_psi = read_psi_memory_some_avg10();
      metrics_.mem_pressure_pct.store(mem_psi, std::memory_order_relaxed);
    }
```

Update the `signals_.update` call:
```cpp
      signals_.update(t, dt_s, smoothing, mem_psi);
```

**Step 3: Update fake_loop to increment new counters**

In `fake_loop` (around line 326-336), add after the existing `blk_write_bytes_total` line:

```cpp
    metrics_.tcp_retransmit_total.fetch_add(std::rand() % 3, std::memory_order_relaxed);
    metrics_.irq_total.fetch_add(500 + (std::rand() % 5000), std::memory_order_relaxed);
    metrics_.mem_pressure_pct.store((double)(std::rand() % 30), std::memory_order_relaxed);
```

**Step 4: Build and test**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add daemon/src/app/app.cpp
git commit -m "feat: read PSI memory pressure and wire new signal counters in sampler loop"
```

---

### Task 10: Add musical mapping for new signals

**Files:**
- Modify: `daemon/src/engine/music.cpp:64-210`

**Step 1: Write test for new signal musical effects**

In `daemon/tests/test_main.cpp`:

```cpp
TEST_CASE(music_retransmit_glitch) {
  // High retx signal should sometimes produce notes even in ambient with low density.
  khor::MusicEngine eng;
  khor::Signal01 s{};
  s.retx = 0.9; // heavy retransmit spike
  s.exec = 0.1; // minimal other activity

  khor::MusicConfig cfg;
  cfg.preset = "ambient";
  cfg.key_midi = 62;
  cfg.density = 0.5;
  cfg.scale = "pentatonic_minor";

  // Run 16 steps (one bar), count notes.
  int note_count = 0;
  for (int i = 0; i < 16; i++) {
    auto frame = eng.tick(s, cfg);
    note_count += (int)frame.notes.size();
  }
  // With retx=0.9 and density=0.5, we should get at least a few glitch notes.
  CHECK(note_count > 0);
}
```

**Step 2: Run test to verify it fails**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: Likely FAIL — with exec=0.1, activity=0.1, and density=0.5 ambient will be very sparse. The retx signal isn't mapped yet so it doesn't help.

**Step 3: Add musical mappings in each preset**

In `daemon/src/engine/music.cpp`, inside `MusicEngine::tick()`:

**Update `activity` calculation** (around line 73) to include new signals:

```cpp
  const double activity = std::max({s.exec, s.rx, s.tx, s.csw, s.io, s.retx, s.irq});
```

**Update synth params** (around line 79-81) to incorporate memory pressure:

```cpp
  // Synth params: map IO to cutoff; map exec to resonance; mem pressure darkens.
  SynthParams sp;
  sp.cutoff01 = (float)clamp01(0.30 + 0.60 * s.io + 0.15 * (s.rx + s.tx) * 0.5 - 0.20 * s.mem);
  sp.resonance01 = (float)clamp01(0.18 + 0.55 * s.exec + 0.15 * s.mem);
```

The `-0.20 * s.mem` makes the filter darker under memory pressure. The `+0.15 * s.mem` on resonance adds a slightly strained quality.

**Add retransmit glitch notes** — insert this block right before `out.synth = sp;` (around line 204), so it applies to ALL presets:

```cpp
  // TCP retransmit glitch: chromatic stab outside the scale.
  if (s.retx > 0.08) {
    const double p_glitch = dens * s.retx * 0.6;
    if (frand01(seed) < p_glitch) {
      // Pick a note that's deliberately off-scale (chromatic).
      int semi = (int)(frand01(seed) * 12.0);
      int oct = 2 + (int)(frand01(seed) * 2.0);
      int midi = std::clamp(cfg.key_midi + semi + oct * 12, 0, 127);
      push_note(out.notes, midi, (float)clamp01(0.25 + 0.60 * s.retx), 0.06f, kChPerc);
    }
  }

  // IRQ texture: very short hi-hat-like notes in high register.
  if (s.irq > 0.10) {
    const double p_tick = dens * s.irq * 0.40;
    if (frand01(seed) < p_tick) {
      int deg = (int)(frand01(seed) * sc.count);
      int midi = pick_note(cfg.key_midi, sc, deg, 4 + (step_ & 1));
      push_note(out.notes, midi, (float)clamp01(0.06 + 0.18 * s.irq), 0.02f, kChPerc);
    }
  }
```

**Update per-preset reverb to respond to memory pressure:**

In the ambient preset (around line 105):
```cpp
    sp.reverb_mix01 = (float)clamp01(0.38 + 0.35 * s.rx + 0.15 * s.mem);
```

In the drone preset (around line 180):
```cpp
    sp.reverb_mix01 = (float)clamp01(0.45 + 0.25 * s.rx + 0.12 * s.mem);
```

**Step 4: Run tests**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add daemon/src/engine/music.cpp daemon/tests/test_main.cpp
git commit -m "feat: map TCP retransmits, IRQ rate, and memory pressure to musical parameters"
```

---

### Task 11: Update OSC metrics encoding and API metrics for new signals

**Files:**
- Modify: `daemon/src/osc/encode.h:64-76` (encode_metrics)
- Modify: `daemon/src/app/app.cpp:259-323` (music_loop OSC signal sends)
- Modify: `daemon/src/app/app.cpp:399-456` (api_metrics)

**Step 1: Update encode_metrics to include new rates**

In `daemon/src/osc/encode.h`:

```cpp
inline std::vector<uint8_t> encode_metrics(const SignalRates& r) {
  std::vector<uint8_t> b;
  b.reserve(160);
  put_str(b, "/khor/metrics");
  put_str(b, ",fffffffff");
  put_f32(b, (float)r.exec_s);
  put_f32(b, (float)r.rx_kbs);
  put_f32(b, (float)r.tx_kbs);
  put_f32(b, (float)r.csw_s);
  put_f32(b, (float)r.blk_r_kbs);
  put_f32(b, (float)r.blk_w_kbs);
  put_f32(b, (float)r.retx_s);
  put_f32(b, (float)r.irq_s);
  put_f32(b, (float)r.mem_pct);
  return b;
}
```

**Step 2: Add new signal names to OSC signal sends in music_loop**

In `daemon/src/app/app.cpp`, in the OSC signal block (around line 312-318), add after the `io` send:

```cpp
        osc_.send_signal("retx", (float)s01.retx);
        osc_.send_signal("irq", (float)s01.irq);
        osc_.send_signal("mem", (float)s01.mem);
```

**Step 3: Add new rates to api_metrics JSON**

In `daemon/src/app/app.cpp`, in `api_metrics` (around line 419-426), add after `blk_w_kbs`:

```cpp
    {"retx_s", JsonValue::make_number(r.retx_s)},
    {"irq_s", JsonValue::make_number(r.irq_s)},
    {"mem_pct", JsonValue::make_number(r.mem_pct)},
```

And in the totals section (around line 403-412), add:

```cpp
    {"tcp_retransmit_total", JsonValue::make_number((double)metrics_.tcp_retransmit_total.load(std::memory_order_relaxed))},
    {"irq_total", JsonValue::make_number((double)metrics_.irq_total.load(std::memory_order_relaxed))},
```

**Step 4: Build and test**

Run: `./scripts/linux-build.sh && ./daemon/build/khor-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add daemon/src/osc/encode.h daemon/src/app/app.cpp
git commit -m "feat: expose new signals in OSC output and HTTP API metrics"
```

---

### Task 12: Final integration build and full test run

**Step 1: Clean build from scratch**

Run: `rm -rf /home/ilia/coding/khor/daemon/build && ./scripts/linux-build.sh`
Expected: Clean build with no warnings related to our changes.

**Step 2: Run all tests**

Run: `./daemon/build/khor-tests`
Expected: All tests pass (original + 3 new: `music_channel_assignment`, `signals_new_counters`, `music_retransmit_glitch`).

**Step 3: Verify the updated OSC test**

The updated `osc_encoding_note` test should pass with the new `,iiff` format.

**Step 4: Commit any remaining fixes**

If any fixes were needed, commit them with descriptive messages.
