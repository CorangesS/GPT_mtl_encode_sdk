#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

// A minimal lock-free SPSC ring. Suitable for MTL callback thread -> app thread.
// NOTE: callback thread must not block (MTL tasklet constraint).

template <typename T>
class SpscRing {
public:
  explicit SpscRing(size_t cap_pow2)
      : cap_(cap_pow2), mask_(cap_pow2 - 1), buf_(cap_pow2) {
    // capacity must be power of two
  }

  bool push(const T& v) {
    auto h = head_.load(std::memory_order_relaxed);
    auto n = (h + 1) & mask_;
    if (n == tail_.load(std::memory_order_acquire)) return false; // full
    buf_[h] = v;
    head_.store(n, std::memory_order_release);
    return true;
  }

  bool pop(T& out) {
    auto t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false; // empty
    out = buf_[t];
    tail_.store((t + 1) & mask_, std::memory_order_release);
    return true;
  }

private:
  size_t cap_;
  size_t mask_;
  std::vector<T> buf_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};
