#pragma once

#include <atomic>
#include <cstdint>

struct KhorMetrics {
  std::atomic<uint64_t> events_total{0};
  std::atomic<uint64_t> events_dropped{0};

  std::atomic<uint64_t> exec_total{0};
  std::atomic<uint64_t> net_rx_bytes_total{0};
  std::atomic<uint64_t> net_tx_bytes_total{0};
  std::atomic<uint64_t> sched_switch_total{0};
  std::atomic<uint64_t> blk_read_bytes_total{0};
  std::atomic<uint64_t> blk_write_bytes_total{0};

  std::atomic<double> bpm{110.0};
  std::atomic<int> key_midi{62}; // D4
};
