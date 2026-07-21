#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>

#include "common/types.hpp"

namespace lab {

// Naive std::map-based order book used two ways:
//  1. As the correctness oracle in randomized differential tests against
//     OrderBook (any divergence in book state fails the test).
//  2. As the cache-unfriendly baseline in benchmarks (node-based containers,
//     pointer chasing, per-insert allocation).
class ReferenceBook {
 public:
  struct RefOrder {
    OrderId id;
    Qty qty;
  };
  // Bids sorted descending so begin() is best on both sides.
  using BidLevels = std::map<Price, std::deque<RefOrder>, std::greater<Price>>;
  using AskLevels = std::map<Price, std::deque<RefOrder>, std::less<Price>>;

  void add(OrderId id, Side side, Price price, Qty qty) {
    if (side == Side::kBid) {
      bids_[price].push_back({id, qty});
    } else {
      asks_[price].push_back({id, qty});
    }
    index_[id] = {side, price};
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    erase_from_level(id, it->second.side, it->second.price);
    index_.erase(it);
    return true;
  }

  bool modify(OrderId id, Price new_price, Qty new_qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Side side = it->second.side;
    const Price old_price = it->second.price;
    if (new_price == old_price) {
      RefOrder* o = find_in_level(id, side, old_price);
      if (new_qty <= o->qty) {
        o->qty = new_qty;
        if (o->qty == 0) {
          erase_from_level(id, side, old_price);
          index_.erase(it);
        }
        return true;
      }
    }
    erase_from_level(id, side, old_price);
    index_.erase(it);
    if (new_qty > 0) add(id, side, new_price, new_qty);
    return true;
  }

  Price best_bid() const {
    return bids_.empty() ? INT64_MIN : bids_.begin()->first;
  }
  Price best_ask() const {
    return asks_.empty() ? INT64_MAX : asks_.begin()->first;
  }

  Qty level_qty(Side side, Price price) const {
    Qty total = 0;
    auto sum = [&](const auto& levels) {
      auto it = levels.find(price);
      if (it != levels.end()) {
        for (const RefOrder& o : it->second) total += o.qty;
      }
    };
    if (side == Side::kBid) {
      sum(bids_);
    } else {
      sum(asks_);
    }
    return total;
  }

  size_t open_orders() const { return index_.size(); }
  const BidLevels& bids() const { return bids_; }
  const AskLevels& asks() const { return asks_; }

 private:
  struct Locator {
    Side side;
    Price price;
  };

  RefOrder* find_in_level(OrderId id, Side side, Price price) {
    auto& q = side == Side::kBid ? bids_[price] : asks_[price];
    for (RefOrder& o : q) {
      if (o.id == id) return &o;
    }
    return nullptr;
  }

  void erase_from_level(OrderId id, Side side, Price price) {
    auto erase = [&](auto& levels) {
      auto lit = levels.find(price);
      auto& q = lit->second;
      for (auto oit = q.begin(); oit != q.end(); ++oit) {
        if (oit->id == id) {
          q.erase(oit);
          break;
        }
      }
      if (q.empty()) levels.erase(lit);
    };
    if (side == Side::kBid) {
      erase(bids_);
    } else {
      erase(asks_);
    }
  }

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lab
