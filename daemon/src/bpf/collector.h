#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "khor/metrics.h"

namespace khor {

struct BpfConfig {
  bool enabled = true;
  uint32_t enabled_mask = 0xFFFFFFFFu;
  uint32_t sample_interval_ms = 200;
  uint32_t tgid_allow = 0;
  uint32_t tgid_deny = 0;
  uint64_t cgroup_id = 0;
};

struct BpfStatus {
  bool enabled = false;
  bool ok = false;
  int err_code = 0; // errno-style negative libbpf error, 0 if ok/disabled
  std::string error;
};

class BpfCollector {
 public:
  BpfCollector();
  ~BpfCollector();

  BpfCollector(const BpfCollector&) = delete;
  BpfCollector& operator=(const BpfCollector&) = delete;

  bool start(const BpfConfig& cfg, KhorMetrics* metrics, std::string* err);
  void stop();

  bool is_running() const;
  BpfStatus status() const;

  // Best-effort live update (mask + interval + filters).
  bool apply_config(const BpfConfig& cfg, std::string* err);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace khor

