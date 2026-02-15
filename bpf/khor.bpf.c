// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "../bpf/khor.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 1 << 24); // 16 MiB
} events SEC(".maps");

static __always_inline void submit_event(uint32_t type, uint64_t a, uint64_t b) {
  struct khor_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
  if (!e) return;

  uint64_t pid_tgid = bpf_get_current_pid_tgid();
  e->ts_ns = bpf_ktime_get_ns();
  e->pid = (uint32_t)pid_tgid;
  e->tgid = (uint32_t)(pid_tgid >> 32);
  e->type = type;
  e->a = a;
  e->b = b;

  bpf_ringbuf_submit(e, 0);
}

SEC("tracepoint/syscalls/sys_enter_execve")
int tp_execve(struct trace_event_raw_sys_enter *ctx) {
  (void)ctx;
  submit_event(KHOR_EV_EXEC, 0, 0);
  return 0;
}

SEC("tracepoint/net/netif_receive_skb")
int tp_net_rx(struct trace_event_raw_net_dev_template *ctx) {
  // ctx->len is available on this template in many kernels, but keep it defensive:
  uint32_t len = 0;
  bpf_core_read(&len, sizeof(len), &ctx->len);
  int32_t ifindex = 0;
  bpf_core_read(&ifindex, sizeof(ifindex), &ctx->ifindex);
  submit_event(KHOR_EV_NET_RX, len, (uint64_t)(uint32_t)ifindex);
  return 0;
}

SEC("tracepoint/net/net_dev_queue")
int tp_net_tx(struct trace_event_raw_net_dev_template *ctx) {
  uint32_t len = 0;
  bpf_core_read(&len, sizeof(len), &ctx->len);
  int32_t ifindex = 0;
  bpf_core_read(&ifindex, sizeof(ifindex), &ctx->ifindex);
  submit_event(KHOR_EV_NET_TX, len, (uint64_t)(uint32_t)ifindex);
  return 0;
}
