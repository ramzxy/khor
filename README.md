# khor

Sonify kernel activity using eBPF: stream structured kernel/network events into a musical engine and play audio directly, with a local React UI for visualization and live control.

## Dev Environment (Linux)

This project is designed for Linux. WSL2 is fine for development as long as your WSL kernel has eBPF + BTF enabled.

### 1) Check eBPF Support

```bash
./scripts/linux-check.sh
```

### 2) Install Build Dependencies

On Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config clang llvm bpftool libbpf-dev linux-headers-$(uname -r) \
  libasound2-dev
```

On Fedora:
```bash
sudo dnf install -y \
  gcc gcc-c++ make cmake pkgconf-pkg-config clang llvm bpftool libbpf-devel
```

Notes:
- `bpftool` is used to generate `vmlinux.h` and the libbpf skeleton header at build time.
- `libasound2-dev` is only needed if you later switch to ALSA directly. The current MVP uses miniaudio (downloaded by script).
- If you see ALSA "Permission denied" errors, ensure your user can access `/dev/snd/*` (common fix: `sudo usermod -aG audio $USER` then restart your session/WSL).
- If you want eBPF without running as root, try: `sudo setcap cap_bpf,cap_perfmon+ep daemon/build/khor-daemon` (use `KHOR_DEBUG_LIBBPF=1` for verbose libbpf errors).

### 3) Fetch Single-Header Deps

```bash
./scripts/fetch_deps.sh
```

### 4) Build + Run

```bash
./scripts/linux-build.sh
./scripts/linux-run.sh
```

If eBPF fails to load as your user, run `sudo ./scripts/wsl-run.sh` once. The script will try to `setcap` the daemon binary (so it can load eBPF) and then drop back to your user for audio.

By default the daemon listens on `http://127.0.0.1:17321`.

## WSL2 Notes (Optional)

If you’re developing on WSL2 and don’t have a distro installed yet:

```powershell
wsl --install -d Ubuntu
```

On WSL2, you may need to run `wsl --update` (from Windows) and restart WSL to get a kernel with eBPF + BTF.

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
