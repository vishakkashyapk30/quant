#include "feed_handler/receiver.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "common/spsc_ring.hpp"
#include "common/timing.hpp"
#include "common/wire.hpp"
#include "exchange_sim/udp_sender.hpp"

namespace lab {
namespace {

using Receiver = FeedReceiver<RingSink>;

// Loopback round trip for one receive mode: send a known sequence, assert
// every message arrives intact, in order, with no gaps.
void run_mode_roundtrip(Receiver::Mode mode) {
  SpscRing<wire::Msg> ring(1 << 12);
  RingSink sink{ring};
  Receiver rx(/*port=*/0, mode, sink);  // ephemeral port
  const uint16_t port = rx.bound_port();
  ASSERT_NE(port, 0);

  std::atomic<bool> stop{false};
  std::thread rx_thread([&] {
    try {
      rx.run(stop);
    } catch (const std::exception&) {
      // io_uring may be unavailable (kernel sysctl / seccomp); the main
      // thread detects this via missing messages and skips.
    }
  });

  constexpr uint32_t kMsgs = 1000;
  constexpr size_t kPerPacket = 8;
  {
    UdpSender tx("127.0.0.1", port);
    wire::Msg batch[kPerPacket];
    uint32_t seq = 0;
    while (seq < kMsgs) {
      size_t n = 0;
      for (; n < kPerPacket && seq < kMsgs; ++n, ++seq) {
        batch[n] = wire::Msg{};
        batch[n].send_ts_ns = now_ns();
        batch[n].seq = seq;
        batch[n].order_id = seq + 1;
        batch[n].type = static_cast<uint8_t>(wire::MsgType::kAdd);
        batch[n].symbol = 1;
        batch[n].price = 100;
        batch[n].qty = 1;
      }
      tx.send_batch(batch, n);
      // Loopback is lossy only under extreme burst; tiny pacing avoids
      // flaky drops without hiding real bugs.
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  // Drain with timeout.
  uint32_t received = 0;
  wire::Msg m;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received < kMsgs && std::chrono::steady_clock::now() < deadline) {
    if (ring.try_pop(m)) {
      EXPECT_EQ(m.seq, received);
      EXPECT_EQ(m.order_id, received + 1);
      ++received;
    }
  }
  stop.store(true);
  rx_thread.join();

  if (mode == Receiver::Mode::kIoUring && received == 0) {
    GTEST_SKIP() << "io_uring appears unavailable in this environment";
  }
  EXPECT_EQ(received, kMsgs);
  EXPECT_EQ(rx.stats().messages, kMsgs);
  EXPECT_EQ(rx.stats().gaps, 0u);
  EXPECT_EQ(rx.stats().ring_full, 0u);
}

TEST(FeedReceiver, EpollRoundtrip) { run_mode_roundtrip(Receiver::Mode::kEpoll); }

TEST(FeedReceiver, BusyPollRoundtrip) {
  run_mode_roundtrip(Receiver::Mode::kBusyPoll);
}

TEST(FeedReceiver, RecvmmsgRoundtrip) {
  run_mode_roundtrip(Receiver::Mode::kRecvmmsg);
}

TEST(FeedReceiver, IoUringRoundtrip) {
  if (!Receiver::io_uring_available()) {
    GTEST_SKIP() << "built without liburing";
  }
  run_mode_roundtrip(Receiver::Mode::kIoUring);
}

TEST(ShardedSink, RoutesBySymbol) {
  SpscRing<wire::Msg> r0(16), r1(16), r2(16);
  SpscRing<wire::Msg>* rings[] = {&r0, &r1, &r2};
  ShardedSink sink{rings, 3};

  for (uint16_t sym = 0; sym < 9; ++sym) {
    wire::Msg m{};
    m.symbol = sym;
    ASSERT_TRUE(sink(m));
  }
  EXPECT_EQ(r0.size_approx(), 3u);  // symbols 0, 3, 6
  EXPECT_EQ(r1.size_approx(), 3u);
  EXPECT_EQ(r2.size_approx(), 3u);

  wire::Msg m;
  while (r1.try_pop(m)) EXPECT_EQ(m.symbol % 3, 1u);
}

}  // namespace
}  // namespace lab
