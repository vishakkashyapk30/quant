#include "orderbook/book.hpp"

#include <gtest/gtest.h>

#include <random>

#include "orderbook/reference_book.hpp"

namespace lab {
namespace {

TEST(OrderBook, AddAndBestPrices) {
  OrderBook book;
  book.add(1, Side::kBid, 100, 10);
  book.add(2, Side::kBid, 101, 5);
  book.add(3, Side::kAsk, 103, 7);
  book.add(4, Side::kAsk, 102, 3);
  EXPECT_EQ(book.best_bid(), 101);
  EXPECT_EQ(book.best_ask(), 102);
  EXPECT_EQ(book.open_orders(), 4u);
}

TEST(OrderBook, CancelRemovesEmptyLevel) {
  OrderBook book;
  book.add(1, Side::kBid, 100, 10);
  book.add(2, Side::kBid, 100, 20);
  EXPECT_TRUE(book.cancel(1));
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_TRUE(book.cancel(2));
  EXPECT_EQ(book.best_bid(), INT64_MIN);
  EXPECT_EQ(book.bids().depth(), 0u);
  EXPECT_FALSE(book.cancel(2)) << "double cancel must fail";
}

TEST(OrderBook, QtyReductionKeepsPriority) {
  OrderBook book;
  book.add(1, Side::kAsk, 100, 10);
  book.add(2, Side::kAsk, 100, 10);
  ASSERT_TRUE(book.modify(1, 100, 4));  // reduce in place
  const Level* lvl = book.asks().best();
  ASSERT_NE(lvl, nullptr);
  EXPECT_EQ(lvl->head->id, 1u) << "reduce must not lose time priority";
  EXPECT_EQ(lvl->total_qty, 14u);
}

TEST(OrderBook, PriceChangeLosesPriority) {
  OrderBook book;
  book.add(1, Side::kAsk, 100, 10);
  book.add(2, Side::kAsk, 101, 10);
  ASSERT_TRUE(book.modify(2, 100, 10));  // join level 100 at the back
  const Level* lvl = book.asks().best();
  ASSERT_NE(lvl, nullptr);
  EXPECT_EQ(lvl->order_count, 2u);
  EXPECT_EQ(lvl->head->id, 1u);
  EXPECT_EQ(lvl->tail->id, 2u);
}

// Differential test: drive both books with the same random operation stream
// and assert identical externally-visible state throughout.
TEST(OrderBook, DifferentialVsReference) {
  OrderBook fast;
  ReferenceBook ref;
  std::mt19937_64 rng(12345);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (int step = 0; step < 200'000; ++step) {
    const int roll = static_cast<int>(rng() % 100);
    if (live.empty() || roll < 55) {
      const OrderId id = next_id++;
      const Side side = (rng() & 1) ? Side::kAsk : Side::kBid;
      const Price px = 1000 + static_cast<Price>(rng() % 100);
      const Qty qty = static_cast<Qty>(rng() % 50) + 1;
      fast.add(id, side, px, qty);
      ref.add(id, side, px, qty);
      live.push_back(id);
    } else if (roll < 80) {
      const size_t i = rng() % live.size();
      const OrderId id = live[i];
      live[i] = live.back();
      live.pop_back();
      EXPECT_EQ(fast.cancel(id), ref.cancel(id));
    } else {
      const size_t i = rng() % live.size();
      const OrderId id = live[i];
      const Price px = 1000 + static_cast<Price>(rng() % 100);
      const Qty qty = static_cast<Qty>(rng() % 50);  // may be 0 => removal
      EXPECT_EQ(fast.modify(id, px, qty), ref.modify(id, px, qty));
      if (qty == 0) {
        live[i] = live.back();
        live.pop_back();
      }
    }

    ASSERT_EQ(fast.best_bid(), ref.best_bid()) << "step " << step;
    ASSERT_EQ(fast.best_ask(), ref.best_ask()) << "step " << step;
    ASSERT_EQ(fast.open_orders(), ref.open_orders()) << "step " << step;
  }

  // Full-depth comparison at the end: every level's aggregate qty matches.
  for (const auto& [px, orders] : ref.bids()) {
    ASSERT_EQ(ref.level_qty(Side::kBid, px),
              [&] {
                for (const Level* l : fast.bids().levels()) {
                  if (l->price == px) return l->total_qty;
                }
                return Qty{0};
              }());
  }
}

TEST(OrderBook, PoolReuseNoGrowthAfterWarmup) {
  OrderBook book;
  // Churn far more orders than the slab size, but keep <= slab live.
  for (OrderId id = 1; id <= 100'000; ++id) {
    book.add(id, Side::kBid, 1000 + static_cast<Price>(id % 50), 10);
    if (id > 100) book.cancel(id - 100);
  }
  EXPECT_EQ(book.order_pool().slab_count(), 1u)
      << "hot-path churn must not allocate new slabs";
}

}  // namespace
}  // namespace lab
