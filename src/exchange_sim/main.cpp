// exchange_sim: streams synthetic order flow over UDP at a configurable rate.
//
// Usage: exchange_sim [host] [port] [msgs_per_sec] [duration_sec] [seed] [symbols]
// Defaults: 127.0.0.1 9100 100000 10 42 1

#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/timing.hpp"
#include "common/wire.hpp"
#include "exchange_sim/generator.hpp"
#include "exchange_sim/udp_sender.hpp"

int main(int argc, char** argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const uint16_t port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 9100;
  const uint64_t rate = argc > 3 ? strtoull(argv[3], nullptr, 10) : 100'000;
  const uint64_t duration_s = argc > 4 ? strtoull(argv[4], nullptr, 10) : 10;
  const uint64_t seed = argc > 5 ? strtoull(argv[5], nullptr, 10) : 42;
  const uint16_t symbols =
      argc > 6 ? static_cast<uint16_t>(atoi(argv[6])) : 1;

  lab::UdpSender sender(host, port);
  lab::FlowGenerator::Config cfg;
  cfg.seed = seed;
  cfg.n_symbols = symbols;
  lab::FlowGenerator gen(cfg);

  // Pace in batches: send a full datagram's worth, then sleep off the
  // schedule slack. Send timestamps are stamped per message immediately
  // before the batch goes out.
  constexpr size_t kBatch = lab::wire::kMaxMsgsPerPacket;
  lab::wire::Msg batch[kBatch];

  const uint64_t start_ns = lab::now_ns();
  const uint64_t end_ns = start_ns + duration_s * 1'000'000'000ull;
  const double ns_per_msg = 1e9 / static_cast<double>(rate);
  uint64_t sent = 0;

  fprintf(stderr, "exchange_sim: %s:%u rate=%lu msg/s duration=%lus seed=%lu\n",
          host.c_str(), port, static_cast<unsigned long>(rate),
          static_cast<unsigned long>(duration_s),
          static_cast<unsigned long>(seed));

  while (true) {
    const uint64_t now = lab::now_ns();
    if (now >= end_ns) break;

    const auto due =
        static_cast<uint64_t>(static_cast<double>(now - start_ns) / ns_per_msg);
    if (due <= sent) {
      timespec req{0, 100'000};  // 100 us
      nanosleep(&req, nullptr);
      continue;
    }

    size_t n = due - sent;
    if (n > kBatch) n = kBatch;
    for (size_t i = 0; i < n; ++i) {
      batch[i] = gen.next();
      batch[i].send_ts_ns = lab::now_ns();
    }
    sender.send_batch(batch, n);
    sent += n;
  }

  const double secs = static_cast<double>(lab::now_ns() - start_ns) / 1e9;
  fprintf(stderr, "exchange_sim: sent %lu msgs in %.2fs (%.0f msg/s)\n",
          static_cast<unsigned long>(sent), secs,
          static_cast<double>(sent) / secs);
  return 0;
}
