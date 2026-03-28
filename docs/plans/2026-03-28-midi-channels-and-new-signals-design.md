# Multi-Channel MIDI + New Kernel Signals

**Date:** 2026-03-28

## Goals

1. Make MIDI output DAW-grade for live use by tagging notes with channels per voice role and fixing note-off collisions
2. Add three new kernel signals (memory pressure, TCP retransmits, IRQ rate) with distinct temporal characters for richer sonification

## Feature 1: Multi-Channel MIDI + Note-Off Fix

### NoteEvent changes

Add `channel` field (1-16, default 1) to `NoteEvent`.

### Fixed channel convention

| Channel | Role | Notes |
|---------|------|-------|
| 1 | Melody | ambient melody, arp arpeggios, drone high sprinkles |
| 2 | Bass | percussive kick, drone root/mid |
| 3 | Chords/Pads | ambient exec dyads, arp chord stabs |
| 10 | Percussion | percussive clicks, percussive mid hits |

### Note-off fix

Track pending note-offs by `(channel, midi)` pair with a reference counter. Only send ALSA note-off when counter reaches zero. This prevents early note-off when the same pitch fires twice before the first release.

### MIDI output changes

- `send_note()` uses `ev.channel` instead of hardcoded channel
- `send_signals_cc()` sends CCs on channel 1 (unchanged)
- Config `midi_channel` becomes fallback for notes without explicit channel

### Audio engine

No changes — internal synth ignores channels. Channel field consumed by MIDI and OSC outputs only.

## Feature 2: New Kernel Signals

### Three new signals

1. **Memory pressure (PSI)** — read `/proc/pressure/memory` in userspace, no eBPF. Polled every ~1s. Slow-moving mood signal.
2. **TCP retransmits** — eBPF tracepoint `tcp/tcp_retransmit_skb`. Spiky event signal.
3. **IRQ rate** — eBPF tracepoint `irq/irq_handler_entry`. Fast texture signal.

### Data flow additions

- `KhorMetrics`: +2 atomics (`tcp_retransmit_total`, `irq_total`), +1 atomic double (`mem_pressure_pct`)
- `khor_sample_payload`: +2 fields (`tcp_retransmits`, `irq_count`), fits in existing union padding
- `khor_probe_mask`: +2 bits (`KHOR_PROBE_TCP = 1<<4`, `KHOR_PROBE_IRQ = 1<<5`)
- `Signal01`: +3 normalized values (`mem`, `retx`, `irq`)
- `SignalRates`: +3 rates (`mem_pct`, `retx_s`, `irq_s`)

### Musical mapping

| Signal | Role | Mapping |
|--------|------|---------|
| `mem` | Mood/tension | Increases reverb darkness, suppresses high notes |
| `retx` | Glitch accent | Short chromatic stab outside scale on spikes |
| `irq` | Texture | Very short hi-hat-like notes, high octave, low velocity |

### PSI reading

Read `/proc/pressure/memory` every ~1s in `sampler_loop`. Parse `some avg10=X.XX`. No eBPF complexity needed.

## Files modified

- `daemon/src/engine/note_event.h` — add channel field
- `daemon/src/engine/music.cpp` — assign channels per voice role in each preset
- `daemon/src/midi/alsa_seq.cpp` — per-channel note-off tracking, use ev.channel
- `daemon/src/osc/osc.cpp` — pass channel in OSC messages
- `daemon/include/khor/metrics.h` — new atomic counters
- `daemon/src/engine/signals.h` — new Signal01/SignalRates fields
- `daemon/src/engine/signals.cpp` — normalize new signals
- `bpf/khor.h` — new probe masks, payload fields
- `bpf/khor.bpf.c` — new tracepoints (tcp_retransmit_skb, irq_handler_entry)
- `daemon/src/bpf/collector.cpp` — consume new payload fields
- `daemon/src/app/app.cpp` — PSI reading in sampler_loop, new signals in music_loop, fake_loop updates
- `daemon/src/engine/music.cpp` — musical mapping for mem/retx/irq
