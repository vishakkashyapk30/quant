#include "common/flat_hash_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

namespace lab {
namespace {

TEST(FlatHashMap, InsertFindErase) {
  FlatHashMap<int> m;
  EXPECT_TRUE(m.insert(1, 10));
  EXPECT_FALSE(m.insert(1, 20)) << "duplicate insert must fail";
  ASSERT_NE(m.find(1), nullptr);
  EXPECT_EQ(*m.find(1), 10);
  EXPECT_TRUE(m.erase(1));
  EXPECT_EQ(m.find(1), nullptr);
  EXPECT_FALSE(m.erase(1));
}

TEST(FlatHashMap, GrowsAndKeepsEntries) {
  FlatHashMap<uint64_t> m(16);
  for (uint64_t k = 1; k <= 10'000; ++k) ASSERT_TRUE(m.insert(k, k * 3));
  EXPECT_EQ(m.size(), 10'000u);
  for (uint64_t k = 1; k <= 10'000; ++k) {
    ASSERT_NE(m.find(k), nullptr) << "key " << k;
    EXPECT_EQ(*m.find(k), k * 3);
  }
}

// Differential test vs std::unordered_map, hammering backward-shift
// deletion which is the trickiest part of open addressing.
TEST(FlatHashMap, DifferentialRandomOps) {
  FlatHashMap<uint64_t> fast(16);
  std::unordered_map<uint64_t, uint64_t> ref;
  std::mt19937_64 rng(3);

  for (int step = 0; step < 200'000; ++step) {
    const uint64_t key = (rng() % 500) + 1;  // small key space => collisions
    switch (rng() % 3) {
      case 0: {
        const uint64_t val = rng();
        EXPECT_EQ(fast.insert(key, val), ref.emplace(key, val).second);
        break;
      }
      case 1: {
        uint64_t* f = fast.find(key);
        auto r = ref.find(key);
        ASSERT_EQ(f == nullptr, r == ref.end()) << "step " << step;
        if (f != nullptr) {
          ASSERT_EQ(*f, r->second);
        }
        break;
      }
      case 2:
        EXPECT_EQ(fast.erase(key), ref.erase(key) > 0);
        break;
    }
    ASSERT_EQ(fast.size(), ref.size());
  }
}

}  // namespace
}  // namespace lab
