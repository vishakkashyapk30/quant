#include "common/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace lab {
namespace {

TEST(SpscRing, PushPopSingleThread) {
  SpscRing<int> ring(8);
  EXPECT_EQ(ring.capacity(), 8u);
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(ring.try_push(i));
  EXPECT_FALSE(ring.try_push(99)) << "ring should be full";
  for (int i = 0; i < 8; ++i) {
    int v = -1;
    EXPECT_TRUE(ring.try_pop(v));
    EXPECT_EQ(v, i);
  }
  int v;
  EXPECT_FALSE(ring.try_pop(v)) << "ring should be empty";
}

TEST(SpscRing, WrapsAroundManyTimes) {
  SpscRing<uint64_t> ring(4);
  for (uint64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(ring.try_push(i));
    uint64_t v;
    ASSERT_TRUE(ring.try_pop(v));
    ASSERT_EQ(v, i);
  }
}

// Cross-thread FIFO integrity: every value arrives exactly once, in order.
// Run under TSan in CI to validate the memory ordering.
TEST(SpscRing, ConcurrentProducerConsumer) {
  constexpr uint64_t kCount = 2'000'000;
  SpscRing<uint64_t> ring(1024);

  std::thread producer([&] {
    for (uint64_t i = 0; i < kCount; ++i) {
      while (!ring.try_push(i)) {
      }
    }
  });

  uint64_t expected = 0;
  while (expected < kCount) {
    uint64_t v;
    if (ring.try_pop(v)) {
      ASSERT_EQ(v, expected);
      ++expected;
    }
  }
  producer.join();
  EXPECT_EQ(expected, kCount);
}

}  // namespace
}  // namespace lab
