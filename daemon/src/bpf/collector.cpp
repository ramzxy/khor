#include "bpf/collector.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../bpf/khor.h"

#if defined(KHOR_HAS_BPF)
#include "khor.skel.h"
#include <bpf/libbpf.h>
#endif

namespace khor {

struct BpfCollector::Impl {
  std::atomic<bool> running{false};
  std::atomic<bool> ok{false};
  std::atomic<int> err_code{0};
  std::string err;

  KhorMetrics* metrics = nullptr;

#if defined(KHOR_HAS_BPF)
  ring_buffer* rb = nullptr;
  khor_bpf* skel = nullptr;
  int cfg_map_fd = -1;
  std::thread poller;
#endif
};

static std::string errno_string(int err) {
  if (err == 0) return "OK";
  int e = err < 0 ? -err : err;
  const char* s = std::strerror(e);
  if (!s) s = "unknown";
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s (errno=%d)", s, e);
  return std::string(buf);
}

BpfCollector::BpfCollector() : impl_(new Impl()) {}
BpfCollector::~BpfCollector() { stop(); delete impl_; impl_ = nullptr; }

bool BpfCollector::is_running() const { return impl_ && impl_->ok.load(); }

BpfStatus BpfCollector::status() const {
  BpfStatus s;
  if (!impl_) return s;
  s.enabled = impl_->running.load();
  s.ok = impl_->ok.load();
  s.err_code = impl_->err_code.load();
  s.error = impl_->err;
  return s;
}

bool BpfCollector::apply_config(const BpfConfig& cfg, std::string* err) {
  if (!impl_) return false;
#if !defined(KHOR_HAS_BPF)
  (void)cfg;
  if (err) *err = "built without eBPF support";
  return false;
#else
  if (!impl_->skel || impl_->cfg_map_fd < 0) {
    if (err) *err = "BPF not running";
    return false;
  }

  khor_bpf_config bcfg{};
  bcfg.enabled_mask = cfg.enabled_mask == 0xFFFFFFFFu ? 0u : cfg.enabled_mask;
  bcfg.sample_interval_ms = cfg.sample_interval_ms;
  bcfg.tgid_allow = cfg.tgid_allow;
  bcfg.tgid_deny = cfg.tgid_deny;
  bcfg.cgroup_id = cfg.cgroup_id;

  uint32_t k = 0;
  int rc = bpf_map_update_elem(impl_->cfg_map_fd, &k, &bcfg, BPF_ANY);
  if (rc != 0) {
    if (err) *err = "failed to update BPF config map: " + errno_string(errno);
    return false;
  }
  return true;
#endif
}

bool BpfCollector::start(const BpfConfig& cfg, KhorMetrics* metrics, std::string* err) {
  if (!impl_) return false;
  stop();

  impl_->metrics = metrics;
  impl_->running.store(cfg.enabled);

  if (!cfg.enabled) {
    impl_->ok.store(false);
    impl_->err_code.store(0);
    impl_->err = "disabled by config";
    return true;
  }

#if !defined(KHOR_HAS_BPF)
  impl_->ok.store(false);
  impl_->err_code.store(0);
  impl_->err = "built without eBPF support";
  if (err) *err = impl_->err;
  return false;
#else
  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

  const bool debug_libbpf = (std::getenv("KHOR_DEBUG_LIBBPF") != nullptr);
  if (debug_libbpf) {
    libbpf_set_print([](enum libbpf_print_level, const char* fmt, va_list args) {
      return std::vfprintf(stderr, fmt, args);
    });
  } else {
    libbpf_set_print([](enum libbpf_print_level, const char*, va_list) { return 0; });
  }

  khor_bpf* skel = khor_bpf__open();
  long open_err = libbpf_get_error(skel);
  if (open_err) {
    impl_->err_code.store((int)-open_err);
    impl_->err = "open failed: " + errno_string((int)open_err);
    if (err) *err = impl_->err;
    return false;
  }
  impl_->skel = skel;

  int rc = khor_bpf__load(skel);
  if (rc) {
    impl_->err_code.store(rc);
    impl_->err = "load failed: " + errno_string(rc) + " (need CAP_BPF/CAP_PERFMON or root)";
    if (err) *err = impl_->err;
    stop();
    return false;
  }

  impl_->cfg_map_fd = bpf_map__fd(skel->maps.khor_cfg);
  if (impl_->cfg_map_fd < 0) {
    impl_->err_code.store(-ENOENT);
    impl_->err = "config map not found";
    if (err) *err = impl_->err;
    stop();
    return false;
  }
  (void)apply_config(cfg, nullptr);

  rc = khor_bpf__attach(skel);
  if (rc) {
    impl_->err_code.store(rc);
    impl_->err = "attach failed: " + errno_string(rc);
    if (err) *err = impl_->err;
    stop();
    return false;
  }

  auto on_event = [](void* ctx, void* data, size_t) -> int {
    auto* m = (KhorMetrics*)ctx;
    auto* e = (const khor_event*)data;
    if (!m || !e) return 0;
    m->events_total.fetch_add(1, std::memory_order_relaxed);
    if (e->type == KHOR_EV_SAMPLE) {
      m->exec_total.fetch_add(e->u.sample.exec_count, std::memory_order_relaxed);
      m->net_rx_bytes_total.fetch_add(e->u.sample.net_rx_bytes, std::memory_order_relaxed);
      m->net_tx_bytes_total.fetch_add(e->u.sample.net_tx_bytes, std::memory_order_relaxed);
      m->sched_switch_total.fetch_add(e->u.sample.sched_switches, std::memory_order_relaxed);
      m->blk_read_bytes_total.fetch_add(e->u.sample.blk_read_bytes, std::memory_order_relaxed);
      m->blk_write_bytes_total.fetch_add(e->u.sample.blk_write_bytes, std::memory_order_relaxed);
      m->events_dropped.fetch_add(e->u.sample.lost_events, std::memory_order_relaxed);
    }
    return 0;
  };

  impl_->rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, impl_->metrics, nullptr);
  if (!impl_->rb) {
    impl_->err_code.store(-ENOMEM);
    impl_->err = "ring buffer init failed";
    if (err) *err = impl_->err;
    stop();
    return false;
  }

  impl_->ok.store(true);
  impl_->err_code.store(0);
  impl_->err.clear();
  std::fprintf(stderr, "khor-daemon: eBPF enabled\n");

  impl_->running.store(true);
  impl_->poller = std::thread([impl = impl_] {
    while (impl->running.load() && impl->ok.load()) {
      int r = ring_buffer__poll(impl->rb, 50 /* ms */);
      if (r == -EINTR) continue;
      if (r < 0) {
        // Poll errors don't mean ringbuf drops, but it's still useful for health.
        impl->err_code.store(r);
        impl->err = "ring_buffer__poll: " + errno_string(r);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
  });

  return true;
#endif
}

void BpfCollector::stop() {
  if (!impl_) return;
  impl_->running.store(false);
#if defined(KHOR_HAS_BPF)
  if (impl_->poller.joinable()) impl_->poller.join();
  if (impl_->rb) ring_buffer__free(impl_->rb);
  impl_->rb = nullptr;
  if (impl_->skel) khor_bpf__destroy(impl_->skel);
  impl_->skel = nullptr;
  impl_->cfg_map_fd = -1;
#endif
  impl_->ok.store(false);
}

} // namespace khor

