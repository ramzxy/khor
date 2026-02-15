# khor

Sonify kernel activity using eBPF: stream structured kernel/network events into a musical engine and play audio directly, with a local React UI for visualization and live control.

## Dev Environment (WSL2)

This project is designed for Linux. WSL2 is fine for development as long as your WSL kernel has eBPF + BTF enabled.

If you don’t have a distro installed yet:

```powershell
wsl --install -d Ubuntu
```

### 1) Check eBPF Support

Run in WSL:

```bash
./scripts/wsl-check.sh
```

### 2) Install Build Dependencies (WSL)

On Ubuntu/Debian (WSL):

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config clang llvm bpftool libbpf-dev linux-headers-$(uname -r) \
  libasound2-dev
```

Notes:
- `bpftool` is used to generate `vmlinux.h` and the libbpf skeleton header at build time.
- `libasound2-dev` is only needed if you later switch to ALSA directly. The current MVP uses miniaudio (downloaded by script).

### 3) Fetch Single-Header Deps (WSL)

```bash
./scripts/fetch_deps.sh
```

### 4) Build + Run (WSL)

```bash
./scripts/wsl-build.sh
./scripts/wsl-run.sh
```

By default the daemon listens on `http://127.0.0.1:17321`.

## UI (Windows or WSL)

The UI lives in `ui/` and talks to the daemon on port `17321`.

```bash
cd ui
npm install
npm run dev
```

## Roadmap (Implementation Order)

1. eBPF probes (tracepoints) -> ringbuf -> userspace collector
2. Music engine (tempo + scale quantizer + density control) -> note events
3. Audio engine (poly synth + delay/reverb + limiter)
4. HTTP API for metrics + control, React UI dashboard
5. Optional adapters: MIDI/OSC out to Ableton/TouchDesigner
