#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace khor {

// Single-producer / single-consumer lock-free ring buffer.
// Capacity must be power-of-two. Push drops when full.
template <typename T, std::size_t CapacityPow2>
class SpscQueue {
  static_assert(CapacityPow2 >= 2, "Capacity too small");
  static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power-of-two");

 public:
  bool push(const T& v) {
    const uint32_t w = w_.load(std::memory_order_relaxed);
    const uint32_t r = r_.load(std::memory_order_acquire);
    if ((w - r) >= CapacityPow2) return false;
    buf_[w & mask_] = v;
    w_.store(w + 1, std::memory_order_release);
    return true;
  }

  bool pop(T* out) {
    if (!out) return false;
    const uint32_t r = r_.load(std::memory_order_relaxed);
    const uint32_t w = w_.load(std::memory_order_acquire);
    if (r == w) return false;
    *out = buf_[r & mask_];
    r_.store(r + 1, std::memory_order_release);
    return true;
  }

  std::size_t approx_size() const {
    const uint32_t w = w_.load(std::memory_order_acquire);
    const uint32_t r = r_.load(std::memory_order_acquire);
    return (std::size_t)(w - r);
  }

 private:
  static constexpr uint32_t mask_ = (uint32_t)(CapacityPow2 - 1);
  std::array<T, CapacityPow2> buf_{};
  std::atomic<uint32_t> w_{0};
  std::atomic<uint32_t> r_{0};
};

} // namespace khor

