#pragma once

#include <cstdint>

namespace lab {

// Prices are integer ticks; converting to/from decimal is a display concern.
using Price = int64_t;
using Qty = uint32_t;
using OrderId = uint64_t;
using SymbolId = uint16_t;

enum class Side : uint8_t { kBid = 0, kAsk = 1 };

constexpr Side opposite(Side s) {
  return s == Side::kBid ? Side::kAsk : Side::kBid;
}

}  // namespace lab
