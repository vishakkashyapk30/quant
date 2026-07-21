#pragma once

#include <cstdint>

#include "orderbook/book.hpp"

namespace lab {

struct Trade {
  OrderId taker_id;
  OrderId maker_id;
  Price price;
  Qty qty;
};

// Price-time priority matching engine over a single OrderBook.
//
// Single-threaded by design: one engine instance owns one symbol's book, and
// symbols are sharded across cores. No locks anywhere on the match path.
// Deterministic: identical input event sequences produce identical trade
// sequences, which the replay tests assert.
template <typename TradeSink>
class Matcher {
 public:
  explicit Matcher(TradeSink& sink, bool huge_pages = false)
      : book_(huge_pages), sink_(sink) {}

  // Processes an incoming order: sweeps the opposite side while it crosses,
  // then rests any remainder. Returns quantity that rested (0 if fully
  // filled or IOC-like exhaustion).
  Qty submit(OrderId id, Side side, Price price, Qty qty) {
    BookSide& opp = book_.side_of(opposite(side));
    // Re-read best() every iteration: execute() frees the maker order and,
    // when the level empties, the level itself.
    while (qty > 0) {
      Level* best = opp.best();
      if (best == nullptr || !crosses(side, price, best->price)) break;
      Order* maker = best->head;  // oldest at this price — time priority
      const Qty fill = qty < maker->qty ? qty : maker->qty;
      sink_(Trade{id, maker->id, best->price, fill});
      qty -= fill;
      book_.execute(maker, fill);
    }
    if (qty > 0) {
      book_.add(id, side, price, qty);
    }
    return qty;
  }

  bool cancel(OrderId id) { return book_.cancel(id); }
  bool modify(OrderId id, Price price, Qty qty) {
    // A modify that crosses is treated as cancel + aggressive re-submit,
    // consistent with priority-losing modifies.
    Order* o = book_.find(id);
    if (o == nullptr) return false;
    const Side side = o->side;
    if (price == o->level->price && qty <= o->qty) {
      return book_.modify(id, price, qty);
    }
    book_.cancel(id);
    if (qty > 0) submit(id, side, price, qty);
    return true;
  }

  OrderBook& book() { return book_; }

 private:
  static bool crosses(Side taker, Price taker_px, Price maker_px) {
    return taker == Side::kBid ? taker_px >= maker_px : taker_px <= maker_px;
  }

  OrderBook book_;
  TradeSink& sink_;
};

}  // namespace lab
