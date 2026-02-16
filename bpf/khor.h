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

// Keep this struct fixed-size and CO-RE friendly (no pointers).
#define KHOR_COMM_LEN 16

enum khor_event_type {
  KHOR_EV_SAMPLE = 1,
};

enum khor_probe_mask {
  KHOR_PROBE_EXEC  = 1u << 0,
  KHOR_PROBE_NET   = 1u << 1,
  KHOR_PROBE_SCHED = 1u << 2,
  KHOR_PROBE_BLOCK = 1u << 3,
};

struct khor_bpf_config {
  khor_u32 enabled_mask;        // bitset of khor_probe_mask (0 => all enabled)
  khor_u32 sample_interval_ms;  // 0 => default
  khor_u32 tgid_allow;          // 0 => allow all
  khor_u32 tgid_deny;           // 0 => deny none
  khor_u64 cgroup_id;           // 0 => off
};

struct khor_sample_payload {
  khor_u64 exec_count;
  khor_u64 net_rx_bytes;
  khor_u64 net_tx_bytes;
  khor_u64 sched_switches;
  khor_u64 blk_read_bytes;
  khor_u64 blk_write_bytes;
  khor_u64 blk_issue_count;
  khor_u64 lost_events; // ringbuf reserve failures since last flush
};

struct khor_event {
  khor_u64 ts_ns;
  khor_u32 pid;
  khor_u32 tgid;
  khor_u32 type;
  khor_u32 cpu;
  char comm[KHOR_COMM_LEN];
  union {
    struct khor_sample_payload sample;
    khor_u64 _u64[8]; // keep event size stable if payload evolves
  } u;
};
