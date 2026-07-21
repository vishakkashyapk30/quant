// book_bench: cache-efficiency benchmark of the flat/intrusive OrderBook
// against the std::map-based ReferenceBook, driven by identical random
// operation streams.
//
// Usage: book_bench [ops] [price_levels] [flat|map|both]
// The third argument restricts the run to one implementation so cache
// profilers (cachegrind, perf stat) can attribute counts to a single book.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "common/timing.hpp"
#include "common/types.hpp"
#include "orderbook/book.hpp"
#include "orderbook/reference_book.hpp"

namespace {

struct Op {
  enum { kAdd, kCancel, kModify } kind;
  lab::OrderId id;
  lab::Side side;
  lab::Price price;
  lab::Qty qty;
};

std::vector<Op> make_ops(size_t n, int64_t levels, uint64_t seed) {
  std::vector<Op> ops;
  ops.reserve(n);
  std::mt19937_64 rng(seed);
  std::vector<lab::OrderId> live;
  lab::OrderId next_id = 1;

  for (size_t i = 0; i < n; ++i) {
    const int roll = static_cast<int>(rng() % 100);
    if (live.empty() || roll < 55) {
      Op op{Op::kAdd, next_id++, (rng() & 1) ? lab::Side::kAsk : lab::Side::kBid,
            1000 + static_cast<lab::Price>(rng() % static_cast<uint64_t>(levels)),
            static_cast<lab::Qty>(rng() % 100) + 1};
      live.push_back(op.id);
      ops.push_back(op);
    } else if (roll < 85) {
      const size_t j = rng() % live.size();
      ops.push_back({Op::kCancel, live[j], lab::Side::kBid, 0, 0});
      live[j] = live.back();
      live.pop_back();
    } else {
      const size_t j = rng() % live.size();
      ops.push_back({Op::kModify, live[j], lab::Side::kBid,
                     1000 + static_cast<lab::Price>(
                                rng() % static_cast<uint64_t>(levels)),
                     static_cast<lab::Qty>(rng() % 100) + 1});
    }
  }
  return ops;
}

template <typename Book>
uint64_t run(Book& book, const std::vector<Op>& ops) {
  const uint64_t t0 = lab::now_ns();
  for (const Op& op : ops) {
    switch (op.kind) {
      case Op::kAdd:
        book.add(op.id, op.side, op.price, op.qty);
        break;
      case Op::kCancel:
        book.cancel(op.id);
        break;
      case Op::kModify:
        book.modify(op.id, op.price, op.qty);
        break;
    }
  }
  return lab::now_ns() - t0;
}

}  // namespace

int main(int argc, char** argv) {
  const size_t ops_n = argc > 1 ? strtoull(argv[1], nullptr, 10) : 5'000'000;
  const int64_t levels = argc > 2 ? atoll(argv[2]) : 100;
  const std::string which = argc > 3 ? argv[3] : "both";
  const bool do_flat = which == "flat" || which == "both";
  const bool do_map = which == "map" || which == "both";

  printf("book_bench: %zu ops, ~%ld price levels per side, book=%s\n", ops_n,
         levels, which.c_str());
  const std::vector<Op> ops = make_ops(ops_n, levels, 42);

  // Interleave rounds to be fair to both books w.r.t. frequency scaling.
  for (int round = 0; round < 3; ++round) {
    if (do_flat) {
      lab::OrderBook fast;
      const uint64_t ns = run(fast, ops);
      printf("round %d  flat OrderBook   : %8.1f ns/op  (%6.2f M ops/s)\n",
             round, static_cast<double>(ns) / static_cast<double>(ops_n),
             static_cast<double>(ops_n) * 1e3 / static_cast<double>(ns));
    }
    if (do_map) {
      lab::ReferenceBook ref;
      const uint64_t ns = run(ref, ops);
      printf("round %d  std::map RefBook : %8.1f ns/op  (%6.2f M ops/s)\n",
             round, static_cast<double>(ns) / static_cast<double>(ops_n),
             static_cast<double>(ops_n) * 1e3 / static_cast<double>(ns));
    }
  }
  return 0;
}
