#include "common/pool_allocator.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "common/huge_alloc.hpp"

namespace lab {
namespace {

struct Payload {
  uint64_t a, b, c, d;
};

TEST(ObjectPool, ConstructDestroyReuse) {
  ObjectPool<Payload> pool(64);
  Payload* p1 = pool.construct(Payload{1, 2, 3, 4});
  EXPECT_EQ(p1->a, 1u);
  EXPECT_EQ(pool.live(), 1u);
  pool.destroy(p1);
  EXPECT_EQ(pool.live(), 0u);
  // Freed node is recycled LIFO.
  Payload* p2 = pool.construct(Payload{5, 6, 7, 8});
  EXPECT_EQ(static_cast<void*>(p2), static_cast<void*>(p1));
  pool.destroy(p2);
}

TEST(ObjectPool, GrowsWhenExhausted) {
  ObjectPool<Payload> pool(8);
  std::vector<Payload*> ptrs;
  for (int i = 0; i < 20; ++i) ptrs.push_back(pool.construct());
  EXPECT_GE(pool.slab_count(), 3u);
  EXPECT_EQ(pool.live(), 20u);
  EXPECT_EQ(pool.high_water(), 20u);
  for (Payload* p : ptrs) pool.destroy(p);
  EXPECT_EQ(pool.live(), 0u);
}

// Huge-page request must always succeed via the fallback chain
// (hugetlb -> THP -> normal pages), whatever the host's configuration.
TEST(ObjectPool, HugePageBackingFallsBackGracefully) {
  ObjectPool<Payload> pool(1 << 16, /*huge_pages=*/true);  // 2 MiB of nodes
  const PageBacking b = pool.backing();
  EXPECT_TRUE(b == PageBacking::kExplicitHuge ||
              b == PageBacking::kTransparentHuge ||
              b == PageBacking::kNormal);
  // And it must actually work as a pool.
  std::vector<Payload*> ptrs;
  for (int i = 0; i < 1000; ++i) ptrs.push_back(pool.construct());
  for (Payload* p : ptrs) pool.destroy(p);
  EXPECT_EQ(pool.live(), 0u);
  EXPECT_EQ(pool.slab_count(), 1u);
}

}  // namespace
}  // namespace lab
