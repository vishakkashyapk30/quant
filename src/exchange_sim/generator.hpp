#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "common/types.hpp"
#include "common/wire.hpp"

namespace lab {

// Synthetic order-flow generator producing a realistic add/cancel/modify mix
// around per-symbol random-walking mid prices. Deterministic for a given
// seed, which the replay tests rely on.
class FlowGenerator {
 public:
  struct Config {
    uint64_t seed = 42;
    uint16_t n_symbols = 1;    // symbols 1..n_symbols
    int64_t mid = 10'000;      // starting mid, in ticks
    int64_t band = 50;         // max offset from mid for new orders
    int add_pct = 60;          // remainder split between cancel and modify
    int cancel_pct = 30;
  };

  explicit FlowGenerator(const Config& cfg) : cfg_(cfg), rng_(cfg.seed) {
    live_.reserve(1 << 16);
    mids_.assign(static_cast<size_t>(cfg.n_symbols) + 1, cfg.mid);
  }

  wire::Msg next() {
    wire::Msg m{};
    m.seq = seq_++;

    const int roll = static_cast<int>(rng_() % 100);

    if (live_.empty() || roll < cfg_.add_pct) {
      m.type = static_cast<uint8_t>(wire::MsgType::kAdd);
      m.order_id = next_id_++;
      m.symbol = static_cast<uint16_t>(1 + rng_() % cfg_.n_symbols);
      m.side = static_cast<uint8_t>(rng_() & 1);
      int64_t& mid = mids_[m.symbol];
      // Random walk the mid occasionally so books don't stagnate.
      if ((rng_() & 0xFF) == 0) mid += static_cast<int64_t>(rng_() % 3) - 1;
      const int64_t off =
          static_cast<int64_t>(rng_() % static_cast<uint64_t>(cfg_.band)) + 1;
      // Mostly passive: bids below mid, asks above. ~5% cross for trades.
      const bool aggressive = (rng_() % 100) < 5;
      if (m.side == 0) {
        m.price = aggressive ? mid + off : mid - off;
      } else {
        m.price = aggressive ? mid - off : mid + off;
      }
      m.qty = static_cast<uint32_t>(rng_() % 100) + 1;
      live_.push_back({m.order_id, m.symbol});
    } else if (roll < cfg_.add_pct + cfg_.cancel_pct) {
      m.type = static_cast<uint8_t>(wire::MsgType::kCancel);
      const LiveOrder o = take_random_live();
      m.order_id = o.id;
      m.symbol = o.symbol;
    } else {
      m.type = static_cast<uint8_t>(wire::MsgType::kModify);
      const size_t i = rng_() % live_.size();
      m.order_id = live_[i].id;
      m.symbol = live_[i].symbol;
      const int64_t mid = mids_[m.symbol];
      const int64_t off =
          static_cast<int64_t>(rng_() % static_cast<uint64_t>(cfg_.band)) + 1;
      m.side = static_cast<uint8_t>(rng_() & 1);
      m.price = m.side == 0 ? mid - off : mid + off;
      m.qty = static_cast<uint32_t>(rng_() % 100) + 1;
    }
    return m;
  }

 private:
  struct LiveOrder {
    OrderId id;
    uint16_t symbol;
  };

  LiveOrder take_random_live() {
    const size_t i = rng_() % live_.size();
    const LiveOrder o = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    return o;
  }

  Config cfg_;
  std::mt19937_64 rng_;
  std::vector<int64_t> mids_;
  uint32_t seq_ = 0;
  OrderId next_id_ = 1;
  std::vector<LiveOrder> live_;
};

}  // namespace lab
