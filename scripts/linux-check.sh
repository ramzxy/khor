#!/usr/bin/env bash
set -euo pipefail

echo "khor: Linux eBPF check"
echo "kernel: $(uname -r)"

if [[ -e /sys/kernel/btf/vmlinux ]]; then
  echo "BTF: /sys/kernel/btf/vmlinux present"
else
  echo "BTF: missing (/sys/kernel/btf/vmlinux). CO-RE builds will not work."
fi

if command -v bpftool >/dev/null 2>&1; then
  echo
  echo "bpftool feature probe (kernel):"
  bpftool feature probe kernel 2>/dev/null | sed -n '1,80p' || true
else
  echo "bpftool: not installed (needed for vmlinux.h + skeleton generation)."
fi

if [[ -r /proc/sys/kernel/unprivileged_bpf_disabled ]]; then
  echo "unprivileged_bpf_disabled: $(cat /proc/sys/kernel/unprivileged_bpf_disabled)"
fi

