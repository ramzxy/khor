// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "khor.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, 1 << 24); // 16 MiB
} events SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct khor_bpf_config);
} khor_cfg SEC(".maps");

struct khor_counters {
  __u64 last_flush_ns;
  struct khor_sample_payload acc;
};

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct khor_counters);
} khor_accum SEC(".maps");

static __always_inline struct khor_bpf_config* get_cfg(void) {
  __u32 k = 0;
  return bpf_map_lookup_elem(&khor_cfg, &k);
}

static __always_inline __u32 cfg_enabled_mask(const struct khor_bpf_config* cfg) {
  const __u32 all = (KHOR_PROBE_EXEC | KHOR_PROBE_NET | KHOR_PROBE_SCHED | KHOR_PROBE_BLOCK);
  if (!cfg) return all;
  return cfg->enabled_mask ? cfg->enabled_mask : all;
}

static __always_inline __u64 cfg_interval_ns(const struct khor_bpf_config* cfg) {
  __u32 ms = cfg ? cfg->sample_interval_ms : 0;
  if (!ms) ms = 200;
  return (__u64)ms * 1000000ULL;
}

static __always_inline bool pass_filters(const struct khor_bpf_config* cfg) {
  if (!cfg) return true;

  __u64 pid_tgid = bpf_get_current_pid_tgid();
  __u32 tgid = (__u32)(pid_tgid >> 32);

  if (cfg->tgid_allow && tgid != cfg->tgid_allow) return false;
  if (cfg->tgid_deny && tgid == cfg->tgid_deny) return false;

  if (cfg->cgroup_id) {
    __u64 cg = bpf_get_current_cgroup_id();
    if (cg != cfg->cgroup_id) return false;
  }

  return true;
}

static __always_inline struct khor_counters* get_counters(void) {
  __u32 k = 0;
  return bpf_map_lookup_elem(&khor_accum, &k);
}

static __always_inline void emit_sample(struct khor_counters* c, const struct khor_bpf_config* cfg, __u64 now) {
  (void)cfg;
  struct khor_event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
  if (!e) {
    c->acc.lost_events++;
    return;
  }

  __u64 pid_tgid = bpf_get_current_pid_tgid();
  e->ts_ns = now;
  e->pid = (__u32)pid_tgid;
  e->tgid = (__u32)(pid_tgid >> 32);
  e->type = KHOR_EV_SAMPLE;
  e->cpu = bpf_get_smp_processor_id();
  bpf_get_current_comm(e->comm, sizeof(e->comm));

  e->u.sample = c->acc;

  bpf_ringbuf_submit(e, 0);
}

static __always_inline void maybe_flush(struct khor_counters* c, const struct khor_bpf_config* cfg, __u64 now) {
  if (!c->last_flush_ns) {
    c->last_flush_ns = now;
    return;
  }

  const __u64 interval_ns = cfg_interval_ns(cfg);
  if (now - c->last_flush_ns < interval_ns) return;

  if (c->acc.exec_count || c->acc.net_rx_bytes || c->acc.net_tx_bytes || c->acc.sched_switches ||
      c->acc.blk_read_bytes || c->acc.blk_write_bytes || c->acc.blk_issue_count || c->acc.lost_events) {
    emit_sample(c, cfg, now);
  }

  c->acc.exec_count = 0;
  c->acc.net_rx_bytes = 0;
  c->acc.net_tx_bytes = 0;
  c->acc.sched_switches = 0;
  c->acc.blk_read_bytes = 0;
  c->acc.blk_write_bytes = 0;
  c->acc.blk_issue_count = 0;
  c->acc.lost_events = 0;
  c->last_flush_ns = now;
}

SEC("tracepoint/syscalls/sys_enter_execve")
int tp_execve(struct trace_event_raw_sys_enter* ctx) {
  (void)ctx;
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_EXEC)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.exec_count++;
  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}

SEC("tracepoint/net/netif_receive_skb")
int tp_net_rx(struct trace_event_raw_net_dev_template* ctx) {
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_NET)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.net_rx_bytes += (__u64)ctx->len;

  // v1.0: ifindex is computed but not yet exported in the sample payload.
  struct sk_buff* skb = (struct sk_buff*)ctx->skbaddr;
  if (skb) {
    (void)BPF_CORE_READ(skb, dev, ifindex);
  }

  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}

SEC("tracepoint/net/net_dev_queue")
int tp_net_tx(struct trace_event_raw_net_dev_template* ctx) {
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_NET)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.net_tx_bytes += (__u64)ctx->len;

  struct sk_buff* skb = (struct sk_buff*)ctx->skbaddr;
  if (skb) {
    (void)BPF_CORE_READ(skb, dev, ifindex);
  }

  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}

SEC("tracepoint/sched/sched_switch")
int tp_sched_switch(struct trace_event_raw_sched_switch* ctx) {
  (void)ctx;
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_SCHED)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  c->acc.sched_switches++;
  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}

SEC("tracepoint/block/block_rq_issue")
int tp_block_rq_issue(struct trace_event_raw_block_rq* ctx) {
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_BLOCK)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  (void)ctx;
  c->acc.blk_issue_count++;

  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}

SEC("tracepoint/block/block_rq_complete")
int tp_block_rq_complete(struct trace_event_raw_block_rq_completion* ctx) {
  const struct khor_bpf_config* cfg = get_cfg();
  if (!pass_filters(cfg)) return 0;
  if (!(cfg_enabled_mask(cfg) & KHOR_PROBE_BLOCK)) return 0;

  struct khor_counters* c = get_counters();
  if (!c) return 0;

  // rwbs is a short string like "R", "W", "WS" etc.
  const char rw = ctx->rwbs[0];
  const __u64 bytes = (__u64)ctx->nr_sector * 512ULL;
  if (rw == 'R') {
    c->acc.blk_read_bytes += bytes;
  } else if (rw == 'W') {
    c->acc.blk_write_bytes += bytes;
  }

  maybe_flush(c, cfg, bpf_ktime_get_ns());
  return 0;
}
