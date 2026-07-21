#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace lab {

// HDR-style log-linear latency histogram.
//
// Layout: 64 exponent buckets (one per bit position of the value), each
// split into 32 linear sub-buckets — relative error is bounded at ~3% while
// the whole structure is a flat 16 KiB array. record() is branch-light and
// allocation-free, safe for the hot path; percentile queries walk the array
// on the cold path.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 5;                 // 32 sub-buckets
  static constexpr int kSub = 1 << kSubBits;
  static constexpr int kBuckets = 64 * kSub;

  void record(uint64_t value_ns) {
    ++counts_[index_of(value_ns)];
    ++total_;
    if (value_ns > max_) max_ = value_ns;
    sum_ += value_ns;
  }

  // p in [0,100]. Returns an upper-bound estimate of the percentile.
  uint64_t percentile(double p) const {
    if (total_ == 0) return 0;
    const uint64_t rank =
        static_cast<uint64_t>(static_cast<double>(total_) * p / 100.0);
    uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen > rank) return upper_bound_of(i);
    }
    return max_;
  }

  uint64_t count() const { return total_; }
  uint64_t max() const { return max_; }
  uint64_t mean() const { return total_ == 0 ? 0 : sum_ / total_; }

  void reset() {
    counts_.fill(0);
    total_ = 0;
    max_ = 0;
    sum_ = 0;
  }

  // Merges another histogram (e.g. per-thread shards) into this one.
  void merge(const LatencyHistogram& other) {
    for (int i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    sum_ += other.sum_;
    if (other.max_ > max_) max_ = other.max_;
  }

 private:
  static int index_of(uint64_t v) {
    if (v < kSub) return static_cast<int>(v);  // exact for tiny values
    const int msb = 63 - __builtin_clzll(v);
    const int exp_bucket = msb - kSubBits + 1;
    const int sub = static_cast<int>((v >> (msb - kSubBits)) & (kSub - 1));
    return exp_bucket * kSub + sub;
  }

  static uint64_t upper_bound_of(int idx) {
    if (idx < kSub) return static_cast<uint64_t>(idx);
    const int exp_bucket = idx / kSub;
    const int sub = idx % kSub;
    const int msb = exp_bucket + kSubBits - 1;
    return ((1ull << kSubBits) + static_cast<uint64_t>(sub) + 1)
               << (msb - kSubBits);
  }

  std::array<uint64_t, kBuckets> counts_{};
  uint64_t total_ = 0;
  uint64_t max_ = 0;
  uint64_t sum_ = 0;
};

}  // namespace lab
