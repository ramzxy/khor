#!/usr/bin/env bash
set -euo pipefail

echo "khor: WSL eBPF check"
echo "kernel: $(uname -r)"

if [[ -e /sys/kernel/btf/vmlinux ]]; then
  echo "BTF: /sys/kernel/btf/vmlinux present"
else
  echo "BTF: missing (/sys/kernel/btf/vmlinux). CO-RE builds will not work."
fi

if command -v bpftool >/dev/null 2>&1; then
  echo
  echo "bpftool feature probe (kernel):"
  # This can be verbose; keep it useful.
  bpftool feature probe kernel 2>/dev/null | sed -n '1,80p' || true
else
  echo "bpftool: not installed (needed for vmlinux.h + skeleton generation)."
fi

if [[ -r /proc/sys/kernel/unprivileged_bpf_disabled ]]; then
  echo "unprivileged_bpf_disabled: $(cat /proc/sys/kernel/unprivileged_bpf_disabled)"
fi

echo
echo "If builds fail on WSL:"
echo "- Run: wsl --update (from Windows) and restart WSL."
echo "- Ensure your WSL kernel supports eBPF + BTF."

