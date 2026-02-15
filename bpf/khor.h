#pragma once

// This header is shared between:
// - userspace (C++): compiled against libc
// - eBPF program (C): compiled with clang -target bpf (no libc headers)
#if defined(__BPF__)
// vmlinux.h (included by the BPF program) provides __u32/__u64.
typedef __u32 khor_u32;
typedef __u64 khor_u64;
#else
#include <stdint.h>
typedef uint32_t khor_u32;
typedef uint64_t khor_u64;
#endif

// Keep this struct small and fixed-size. No strings in the MVP.
enum khor_event_type {
  KHOR_EV_EXEC = 1,
  KHOR_EV_NET_RX = 2,
  KHOR_EV_NET_TX = 3,
};

struct khor_event {
  khor_u64 ts_ns;
  khor_u32 pid;
  khor_u32 tgid;
  khor_u32 type;
  khor_u32 _pad;
  khor_u64 a; // type-specific payload (e.g., bytes)
  khor_u64 b; // type-specific payload (e.g., ifindex)
};
