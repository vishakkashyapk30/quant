#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace lab {

// Single-producer / single-consumer lock-free ring buffer.
//
// Design notes:
// - Indices are monotonically increasing uint64_t; the slot is index & mask.
//   This avoids the classic "one empty slot" waste and makes full/empty
//   checks trivial (head - tail == capacity / head == tail).
// - head_ (producer-owned) and tail_ (consumer-owned) live on separate cache
//   lines to prevent false sharing; each side also keeps a *cached* copy of
//   the other side's index so the common case does not touch the other
//   core's cache line at all.
// - Only release/acquire ordering is needed: the producer's release store on
//   head_ publishes the slot write, the consumer's acquire load observes it.
template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing requires trivially copyable payloads");

 public:
  explicit SpscRing(size_t capacity)
      : mask_(capacity - 1),
        buf_(static_cast<T*>(::operator new(
            capacity * sizeof(T), std::align_val_t{alignof(T)}))) {
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0 &&
           "capacity must be a power of two");
  }

  ~SpscRing() { ::operator delete(buf_, std::align_val_t{alignof(T)}); }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  bool try_push(const T& v) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - cached_tail_ > mask_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - cached_tail_ > mask_) return false;  // genuinely full
    }
    buf_[head & mask_] = v;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;  // genuinely empty
    }
    out = buf_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  size_t capacity() const { return mask_ + 1; }

  size_t size_approx() const {
    return static_cast<size_t>(head_.load(std::memory_order_acquire) -
                               tail_.load(std::memory_order_acquire));
  }

 private:
  const uint64_t mask_;
  T* const buf_;

  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) uint64_t cached_tail_{0};  // producer-local
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) uint64_t cached_head_{0};  // consumer-local
};

}  // namespace lab
