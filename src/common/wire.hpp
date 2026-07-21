#pragma once

#include <cstddef>
#include <cstdint>

namespace lab::wire {

// Deliberately simple fixed-size binary format (ITCH-in-spirit, not any
// vendor's actual protocol). Fields are ordered largest-first so natural
// alignment holds without packed structs; total size is 40 bytes with 4
// trailing pad bytes. Little-endian x86 host order is assumed end to end.
enum class MsgType : uint8_t {
  kAdd = 1,
  kCancel = 2,
  kModify = 3,
};

struct Msg {
  uint64_t send_ts_ns;  // CLOCK_MONOTONIC at send; basis for tick-to-trade
  uint64_t order_id;
  int64_t price;        // integer ticks
  uint32_t seq;         // per-stream sequence, for gap detection
  uint32_t qty;
  uint16_t symbol;
  uint8_t type;         // MsgType
  uint8_t side;         // 0 = bid, 1 = ask
};
static_assert(sizeof(Msg) == 40, "wire layout changed");
static_assert(alignof(Msg) == 8, "wire alignment changed");

// One UDP datagram carries 1..kMaxMsgsPerPacket messages back to back.
constexpr size_t kMaxMsgsPerPacket = 32;
constexpr size_t kMaxPacketBytes = kMaxMsgsPerPacket * sizeof(Msg);

}  // namespace lab::wire
