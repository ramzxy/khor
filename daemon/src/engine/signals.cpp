#include "engine/signals.h"

#include <algorithm>
#include <cmath>

namespace khor {
namespace {

static double clamp01(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

static double norm_log(double v, double v_max) {
  v = std::max(0.0, v);
  v_max = std::max(1e-9, v_max);
  return clamp01(std::log1p(v) / std::log1p(v_max));
}

static double ema(double prev, double x, double alpha) {
  // alpha=0 -> no smoothing, alpha=1 -> very smooth (but never fully frozen).
  alpha = clamp01(alpha) * 0.98;
  return alpha * prev + (1.0 - alpha) * x;
}

} // namespace

void Signals::update(const Totals& cur, double dt_s, double smoothing01, double mem_pressure_pct) {
  cur_ = cur;
  if (!has_prev_) {
    prev_ = cur;
    has_prev_ = true;
    rates_ = SignalRates{};
    v01_ = Signal01{};
    return;
  }

  if (dt_s <= 0.0) dt_s = 0.1;

  rates_.exec_s = (double)(cur.exec_total - prev_.exec_total) / dt_s;
  rates_.rx_kbs = (double)(cur.net_rx_bytes_total - prev_.net_rx_bytes_total) / dt_s / 1024.0;
  rates_.tx_kbs = (double)(cur.net_tx_bytes_total - prev_.net_tx_bytes_total) / dt_s / 1024.0;
  rates_.csw_s = (double)(cur.sched_switch_total - prev_.sched_switch_total) / dt_s;
  rates_.blk_r_kbs = (double)(cur.blk_read_bytes_total - prev_.blk_read_bytes_total) / dt_s / 1024.0;
  rates_.blk_w_kbs = (double)(cur.blk_write_bytes_total - prev_.blk_write_bytes_total) / dt_s / 1024.0;
  rates_.retx_s = (double)(cur.tcp_retransmit_total - prev_.tcp_retransmit_total) / dt_s;
  rates_.irq_s = (double)(cur.irq_total - prev_.irq_total) / dt_s;
  rates_.mem_pct = mem_pressure_pct;

  const double exec01 = norm_log(rates_.exec_s, 250.0);
  const double rx01 = norm_log(rates_.rx_kbs, 50000.0);
  const double tx01 = norm_log(rates_.tx_kbs, 50000.0);
  const double csw01 = norm_log(rates_.csw_s, 120000.0);
  const double io01 = norm_log(rates_.blk_r_kbs + rates_.blk_w_kbs, 80000.0);
  const double retx01 = norm_log(rates_.retx_s, 50.0);     // 50 retx/sec is severe
  const double irq01 = norm_log(rates_.irq_s, 200000.0);   // 200k IRQs/sec is busy
  const double mem01 = clamp01(mem_pressure_pct / 100.0);   // already 0-100, just scale

  v01_.exec = ema(v01_.exec, exec01, smoothing01);
  v01_.rx = ema(v01_.rx, rx01, smoothing01);
  v01_.tx = ema(v01_.tx, tx01, smoothing01);
  v01_.csw = ema(v01_.csw, csw01, smoothing01);
  v01_.io = ema(v01_.io, io01, smoothing01);
  v01_.retx = ema(v01_.retx, retx01, smoothing01 * 0.5); // less smoothing for spiky signal
  v01_.irq = ema(v01_.irq, irq01, smoothing01);
  v01_.mem = ema(v01_.mem, mem01, 0.95);                  // very smooth, slow-moving

  prev_ = cur;
}

} // namespace khor
