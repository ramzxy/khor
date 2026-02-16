#pragma once

#include <cstdint>

namespace khor {

struct SignalRates {
  double exec_s = 0.0;
  double rx_kbs = 0.0;
  double tx_kbs = 0.0;
  double csw_s = 0.0;
  double blk_r_kbs = 0.0;
  double blk_w_kbs = 0.0;
};

struct Signal01 {
  // 0..1 values after normalization + smoothing.
  double exec = 0.0;
  double rx = 0.0;
  double tx = 0.0;
  double csw = 0.0;
  double io = 0.0;
};

// Converts monotonically increasing counters into rates and stable 0..1 signals.
class Signals {
 public:
  struct Totals {
    uint64_t exec_total = 0;
    uint64_t net_rx_bytes_total = 0;
    uint64_t net_tx_bytes_total = 0;
    uint64_t sched_switch_total = 0;
    uint64_t blk_read_bytes_total = 0;
    uint64_t blk_write_bytes_total = 0;
  };

  void update(const Totals& cur, double dt_s, double smoothing01);

  Totals totals() const { return cur_; }
  SignalRates rates() const { return rates_; }
  Signal01 value01() const { return v01_; }

 private:
  Totals cur_{};
  Totals prev_{};
  bool has_prev_ = false;

  SignalRates rates_{};
  Signal01 v01_{};
};

} // namespace khor

