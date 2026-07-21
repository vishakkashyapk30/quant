#pragma once

#include <cstdint>
#include <vector>

#include "common/flat_hash_map.hpp"
#include "common/pool_allocator.hpp"
#include "common/types.hpp"

namespace lab {

struct Level;

// Resting order. Lives in the ObjectPool and is chained into its price
// level's intrusive FIFO (price-time priority: head is oldest).
struct Order {
  OrderId id;
  Qty qty;
  Side side;
  Level* level;
  Order* prev;
  Order* next;
};

struct Level {
  Price price;
  Qty total_qty;
  uint32_t order_count;
  Order* head;  // oldest — first to trade
  Order* tail;  // newest
};

// One side of the book: price levels kept in a sorted flat vector of
// pointers, with the BEST price at the BACK.
//
// Why a vector and not std::map: activity clusters near the top of book, so
// inserts/erases usually shift only a few contiguous pointers (one or two
// cache lines), and best-price access is a single back() read. The
// benchmark suite quantifies this against a std::map baseline.
class BookSide {
 public:
  explicit BookSide(Side side) : side_(side) { levels_.reserve(256); }

  Level* best() const { return levels_.empty() ? nullptr : levels_.back(); }

  // Finds the level for `price`, creating it (via pool) if absent.
  Level* get_or_create(Price price, ObjectPool<Level>& pool) {
    const size_t pos = lower_bound(price);
    if (pos < levels_.size() && levels_[pos]->price == price) {
      return levels_[pos];
    }
    Level* lvl = pool.construct(Level{price, 0, 0, nullptr, nullptr});
    levels_.insert(levels_.begin() + static_cast<ptrdiff_t>(pos), lvl);
    return lvl;
  }

  void remove(Level* lvl, ObjectPool<Level>& pool) {
    const size_t pos = lower_bound(lvl->price);
    levels_.erase(levels_.begin() + static_cast<ptrdiff_t>(pos));
    pool.destroy(lvl);
  }

  size_t depth() const { return levels_.size(); }
  const std::vector<Level*>& levels() const { return levels_; }

 private:
  // Index of the first slot whose price is not "worse" than `price` in this
  // side's ordering (bids ascend, asks descend; best is always at back).
  size_t lower_bound(Price price) const {
    size_t lo = 0, hi = levels_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      const bool mid_worse = side_ == Side::kBid ? levels_[mid]->price < price
                                                 : levels_[mid]->price > price;
      if (mid_worse) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  Side side_;
  std::vector<Level*> levels_;
};

// Per-symbol limit order book. All mutation goes through add/cancel/modify/
// reduce; the matching engine drives executions through reduce() +
// pop-from-head. Zero heap allocation on the hot path once pools are warm.
class OrderBook {
 public:
  explicit OrderBook(bool huge_pages = false)
      : bids_(Side::kBid),
        asks_(Side::kAsk),
        order_pool_(1 << 14, huge_pages),
        level_pool_(1 << 10, huge_pages),
        orders_(1 << 15) {}

  // Rests a new order. Precondition: id is unique, qty > 0. Returns the
  // resting order (never matches — crossing is the matching engine's job).
  Order* add(OrderId id, Side side, Price price, Qty qty) {
    Level* lvl = side_of(side).get_or_create(price, level_pool_);
    Order* o = order_pool_.construct(
        Order{id, qty, side, lvl, lvl->tail, nullptr});
    if (lvl->tail != nullptr) {
      lvl->tail->next = o;
    } else {
      lvl->head = o;
    }
    lvl->tail = o;
    lvl->total_qty += qty;
    ++lvl->order_count;
    orders_.insert(id, o);
    return o;
  }

  bool cancel(OrderId id) {
    Order** slot = orders_.find(id);
    if (slot == nullptr) return false;
    unlink_and_free(*slot);
    orders_.erase(id);
    return true;
  }

  // In-place quantity reduction keeps time priority; anything else
  // (price change / qty increase) is cancel + re-add losing priority,
  // which mirrors real exchange semantics.
  bool modify(OrderId id, Price new_price, Qty new_qty) {
    Order** slot = orders_.find(id);
    if (slot == nullptr) return false;
    Order* o = *slot;
    if (new_price == o->level->price && new_qty <= o->qty) {
      o->level->total_qty -= (o->qty - new_qty);
      o->qty = new_qty;
      if (o->qty == 0) {
        unlink_and_free(o);
        orders_.erase(id);
      }
      return true;
    }
    const Side side = o->side;
    unlink_and_free(o);
    orders_.erase(id);
    if (new_qty > 0) add(id, side, new_price, new_qty);
    return true;
  }

  // Executes `qty` against a resting order (must be the level head to
  // respect time priority; asserted by the matching engine's usage).
  // Returns remaining quantity of the resting order.
  Qty execute(Order* o, Qty qty) {
    o->qty -= qty;
    o->level->total_qty -= qty;
    if (o->qty == 0) {
      const OrderId id = o->id;
      unlink_and_free(o);
      orders_.erase(id);
      return 0;
    }
    return o->qty;
  }

  Order* find(OrderId id) {
    Order** slot = orders_.find(id);
    return slot == nullptr ? nullptr : *slot;
  }

  BookSide& side_of(Side s) { return s == Side::kBid ? bids_ : asks_; }
  const BookSide& bids() const { return bids_; }
  const BookSide& asks() const { return asks_; }

  Price best_bid() const {
    const Level* l = bids_.best();
    return l != nullptr ? l->price : INT64_MIN;
  }
  Price best_ask() const {
    const Level* l = asks_.best();
    return l != nullptr ? l->price : INT64_MAX;
  }

  size_t open_orders() const { return orders_.size(); }
  const ObjectPool<Order>& order_pool() const { return order_pool_; }
  const ObjectPool<Level>& level_pool() const { return level_pool_; }

 private:
  void unlink_and_free(Order* o) {
    Level* lvl = o->level;
    const Side side = o->side;
    if (o->prev != nullptr) {
      o->prev->next = o->next;
    } else {
      lvl->head = o->next;
    }
    if (o->next != nullptr) {
      o->next->prev = o->prev;
    } else {
      lvl->tail = o->prev;
    }
    lvl->total_qty -= o->qty;
    --lvl->order_count;
    order_pool_.destroy(o);
    if (lvl->order_count == 0) {
      side_of(side).remove(lvl, level_pool_);
    }
  }

  BookSide bids_;
  BookSide asks_;
  ObjectPool<Order> order_pool_;
  ObjectPool<Level> level_pool_;
  FlatHashMap<Order*> orders_;
};

}  // namespace lab
