#include "monitoring/histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

namespace lab {
namespace {

TEST(LatencyHistogram, ExactForSmallValues) {
  LatencyHistogram h;
  for (uint64_t v = 0; v < 32; ++v) h.record(v);
  EXPECT_EQ(h.count(), 32u);
  EXPECT_EQ(h.max(), 31u);
  EXPECT_EQ(h.percentile(0), 0u);
}

TEST(LatencyHistogram, BoundedRelativeError) {
  LatencyHistogram h;
  std::mt19937_64 rng(1);
  std::vector<uint64_t> values;
  for (int i = 0; i < 100'000; ++i) {
    // Log-uniform spread: 100ns .. ~100ms
    const double exp = 2.0 + (static_cast<double>(rng() % 10'000) / 10'000) * 6.0;
    values.push_back(static_cast<uint64_t>(std::pow(10.0, exp)));
    h.record(values.back());
  }
  std::sort(values.begin(), values.end());

  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const uint64_t exact =
        values[static_cast<size_t>(static_cast<double>(values.size()) * p / 100.0)];
    const uint64_t est = h.percentile(p);
    const double rel_err =
        std::abs(static_cast<double>(est) - static_cast<double>(exact)) /
        static_cast<double>(exact);
    EXPECT_LT(rel_err, 0.05) << "p" << p << " exact=" << exact
                             << " est=" << est;
  }
}

TEST(LatencyHistogram, MergeMatchesCombinedRecording) {
  LatencyHistogram a, b, combined;
  std::mt19937_64 rng(2);
  for (int i = 0; i < 10'000; ++i) {
    const uint64_t v = rng() % 1'000'000;
    ((i & 1) ? a : b).record(v);
    combined.record(v);
  }
  a.merge(b);
  EXPECT_EQ(a.count(), combined.count());
  EXPECT_EQ(a.max(), combined.max());
  EXPECT_EQ(a.percentile(50), combined.percentile(50));
  EXPECT_EQ(a.percentile(99), combined.percentile(99));
}

}  // namespace
}  // namespace lab
