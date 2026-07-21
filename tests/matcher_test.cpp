#include "matching_engine/matcher.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "exchange_sim/generator.hpp"

namespace lab {
namespace {

struct TradeLog {
  std::vector<Trade> trades;
  void operator()(const Trade& t) { trades.push_back(t); }
};

TEST(Matcher, RestsWhenNoCross) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  EXPECT_EQ(m.submit(1, Side::kBid, 100, 10), 10u);
  EXPECT_EQ(m.submit(2, Side::kAsk, 101, 10), 10u);
  EXPECT_TRUE(log.trades.empty());
}

TEST(Matcher, FullFillAtMakerPrice) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  m.submit(1, Side::kAsk, 100, 10);
  EXPECT_EQ(m.submit(2, Side::kBid, 105, 10), 0u);
  ASSERT_EQ(log.trades.size(), 1u);
  EXPECT_EQ(log.trades[0].price, 100) << "trade executes at maker price";
  EXPECT_EQ(log.trades[0].maker_id, 1u);
  EXPECT_EQ(log.trades[0].taker_id, 2u);
  EXPECT_EQ(m.book().open_orders(), 0u);
}

TEST(Matcher, PartialFillRestsRemainder) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  m.submit(1, Side::kAsk, 100, 4);
  EXPECT_EQ(m.submit(2, Side::kBid, 100, 10), 6u);
  EXPECT_EQ(m.book().best_bid(), 100);
  EXPECT_EQ(m.book().best_ask(), INT64_MAX);
}

TEST(Matcher, TimePriorityWithinLevel) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  m.submit(1, Side::kAsk, 100, 5);
  m.submit(2, Side::kAsk, 100, 5);
  m.submit(3, Side::kBid, 100, 7);
  ASSERT_EQ(log.trades.size(), 2u);
  EXPECT_EQ(log.trades[0].maker_id, 1u) << "older order fills first";
  EXPECT_EQ(log.trades[0].qty, 5u);
  EXPECT_EQ(log.trades[1].maker_id, 2u);
  EXPECT_EQ(log.trades[1].qty, 2u);
}

TEST(Matcher, PricePriorityAcrossLevels) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  m.submit(1, Side::kAsk, 102, 5);
  m.submit(2, Side::kAsk, 100, 5);  // better ask
  m.submit(3, Side::kBid, 102, 8);
  ASSERT_EQ(log.trades.size(), 2u);
  EXPECT_EQ(log.trades[0].maker_id, 2u) << "best price fills first";
  EXPECT_EQ(log.trades[0].price, 100);
  EXPECT_EQ(log.trades[1].maker_id, 1u);
  EXPECT_EQ(log.trades[1].price, 102);
}

TEST(Matcher, SweepEmptiesLevelThenStops) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  m.submit(1, Side::kAsk, 100, 3);
  m.submit(2, Side::kAsk, 100, 3);
  m.submit(3, Side::kAsk, 105, 3);
  EXPECT_EQ(m.submit(4, Side::kBid, 101, 10), 4u) << "stops at non-crossing 105";
  EXPECT_EQ(log.trades.size(), 2u);
  EXPECT_EQ(m.book().best_ask(), 105);
  EXPECT_EQ(m.book().best_bid(), 101);
}

// Determinism: the same generated event stream must always produce the
// identical trade tape. This is the replay-test backbone.
TEST(Matcher, DeterministicReplay) {
  auto run = [] {
    TradeLog log;
    Matcher<TradeLog> m(log);
    FlowGenerator::Config cfg;
    cfg.seed = 7;
    FlowGenerator gen(cfg);
    for (int i = 0; i < 100'000; ++i) {
      const wire::Msg msg = gen.next();
      const Side side = msg.side == 0 ? Side::kBid : Side::kAsk;
      switch (static_cast<wire::MsgType>(msg.type)) {
        case wire::MsgType::kAdd:
          m.submit(msg.order_id, side, msg.price, msg.qty);
          break;
        case wire::MsgType::kCancel:
          m.cancel(msg.order_id);
          break;
        case wire::MsgType::kModify:
          m.modify(msg.order_id, msg.price, msg.qty);
          break;
      }
    }
    return log.trades;
  };

  const std::vector<Trade> a = run();
  const std::vector<Trade> b = run();
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(a.size(), 0u) << "generator must produce some crossing flow";
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].taker_id, b[i].taker_id);
    EXPECT_EQ(a[i].maker_id, b[i].maker_id);
    EXPECT_EQ(a[i].price, b[i].price);
    EXPECT_EQ(a[i].qty, b[i].qty);
  }
}

// Invariant: after any event sequence the book is never crossed.
TEST(Matcher, BookNeverCrossedUnderRandomFlow) {
  TradeLog log;
  Matcher<TradeLog> m(log);
  FlowGenerator::Config cfg;
  cfg.seed = 99;
  FlowGenerator gen(cfg);
  for (int i = 0; i < 50'000; ++i) {
    const wire::Msg msg = gen.next();
    const Side side = msg.side == 0 ? Side::kBid : Side::kAsk;
    switch (static_cast<wire::MsgType>(msg.type)) {
      case wire::MsgType::kAdd:
        m.submit(msg.order_id, side, msg.price, msg.qty);
        break;
      case wire::MsgType::kCancel:
        m.cancel(msg.order_id);
        break;
      case wire::MsgType::kModify:
        m.modify(msg.order_id, msg.price, msg.qty);
        break;
    }
    ASSERT_LT(m.book().best_bid(), m.book().best_ask())
        << "book crossed at step " << i;
  }
}

}  // namespace
}  // namespace lab
