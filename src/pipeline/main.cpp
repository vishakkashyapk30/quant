// latency_lab: receives the UDP feed, runs it through per-symbol order books
// + matching engines, and reports per-stage latency percentiles.
//
// Threading: one receiver thread parses datagrams and routes each message by
// symbol to one SPSC ring per engine thread; each engine owns its shard's
// books outright (no locks anywhere).
//
// Usage: latency_lab [--port N] [--mode epoll|busy-poll|recvmmsg|io_uring]
//                    [--duration S] [--engines N] [--rx-cpu N]
//                    [--engine-cpu0 N] [--huge-pages]
//
// Engine i is pinned to engine-cpu0 + i when --engine-cpu0 is given.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "common/affinity.hpp"
#include "common/flat_hash_map.hpp"
#include "common/spsc_ring.hpp"
#include "common/timing.hpp"
#include "common/wire.hpp"
#include "feed_handler/receiver.hpp"
#include "matching_engine/matcher.hpp"
#include "monitoring/histogram.hpp"
#include "monitoring/reporter.hpp"

namespace {

using RxMode = lab::FeedReceiver<lab::ShardedSink>::Mode;

struct Args {
  uint16_t port = 9100;
  RxMode mode = RxMode::kEpoll;
  uint64_t duration_s = 15;
  int engines = 1;
  int rx_cpu = -1;
  int engine_cpu0 = -1;
  bool huge_pages = false;
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--port") && i + 1 < argc) {
      a.port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
      const char* m = argv[++i];
      if (!strcmp(m, "epoll")) {
        a.mode = RxMode::kEpoll;
      } else if (!strcmp(m, "busy-poll")) {
        a.mode = RxMode::kBusyPoll;
      } else if (!strcmp(m, "recvmmsg")) {
        a.mode = RxMode::kRecvmmsg;
      } else if (!strcmp(m, "io_uring")) {
        a.mode = RxMode::kIoUring;
      } else {
        fprintf(stderr, "unknown mode: %s\n", m);
        exit(2);
      }
    } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
      a.duration_s = strtoull(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "--engines") && i + 1 < argc) {
      a.engines = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--rx-cpu") && i + 1 < argc) {
      a.rx_cpu = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--engine-cpu0") && i + 1 < argc) {
      a.engine_cpu0 = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--huge-pages")) {
      a.huge_pages = true;
    } else if (!strcmp(argv[i], "--busy-poll")) {  // kept for compatibility
      a.mode = RxMode::kBusyPoll;
    } else {
      fprintf(stderr, "unknown arg: %s\n", argv[i]);
      exit(2);
    }
  }
  return a;
}

struct TradeCounter {
  uint64_t count = 0;
  void operator()(const lab::Trade&) { ++count; }
};

// One engine thread's world: its ring, matchers per symbol, histograms.
struct Engine {
  explicit Engine(bool huge_pages)
      : ring(1 << 16), huge(huge_pages) {}

  lab::SpscRing<lab::wire::Msg> ring;
  bool huge;
  TradeCounter trades;
  lab::LatencyHistogram wire_to_engine;
  lab::LatencyHistogram match_time;
  uint64_t processed = 0;
  uint64_t open_orders = 0;

  void run(const std::atomic<bool>& stop, uint64_t end_ns) {
    // Per-symbol matcher shard, created lazily as symbols appear.
    std::vector<std::unique_ptr<lab::Matcher<TradeCounter>>> matchers;
    auto matcher_for = [&](uint16_t symbol) -> lab::Matcher<TradeCounter>& {
      if (symbol >= matchers.size()) matchers.resize(symbol + 1);
      if (!matchers[symbol]) {
        matchers[symbol] =
            std::make_unique<lab::Matcher<TradeCounter>>(trades, huge);
      }
      return *matchers[symbol];
    };

    lab::wire::Msg m;
    while (lab::now_ns() < end_ns && !stop.load(std::memory_order_relaxed)) {
      if (!ring.try_pop(m)) {
        lab::cpu_pause();
        continue;
      }
      const uint64_t t0 = lab::now_ns();
      wire_to_engine.record(t0 - m.send_ts_ns);

      lab::Matcher<TradeCounter>& matcher = matcher_for(m.symbol);
      switch (static_cast<lab::wire::MsgType>(m.type)) {
        case lab::wire::MsgType::kAdd:
          matcher.submit(m.order_id,
                         m.side == 0 ? lab::Side::kBid : lab::Side::kAsk,
                         m.price, m.qty);
          break;
        case lab::wire::MsgType::kCancel:
          matcher.cancel(m.order_id);
          break;
        case lab::wire::MsgType::kModify:
          matcher.modify(m.order_id, m.price, m.qty);
          break;
      }
      match_time.record(lab::now_ns() - t0);
      ++processed;
    }
    for (const auto& mp : matchers) {
      if (mp) open_orders += mp->book().open_orders();
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse_args(argc, argv);
  if (args.mode == RxMode::kIoUring &&
      !lab::FeedReceiver<lab::ShardedSink>::io_uring_available()) {
    fprintf(stderr, "built without liburing; io_uring mode unavailable\n");
    return 2;
  }

  std::vector<std::unique_ptr<Engine>> engines;
  std::vector<lab::SpscRing<lab::wire::Msg>*> rings;
  for (int i = 0; i < args.engines; ++i) {
    engines.push_back(std::make_unique<Engine>(args.huge_pages));
    rings.push_back(&engines.back()->ring);
  }

  lab::ShardedSink sink{rings.data(), rings.size()};
  lab::FeedReceiver<lab::ShardedSink> receiver(args.port, args.mode, sink);

  fprintf(stderr, "latency_lab: port=%u mode=%s engines=%d huge_pages=%d\n",
          args.port,
          lab::FeedReceiver<lab::ShardedSink>::mode_name(args.mode),
          args.engines, args.huge_pages ? 1 : 0);

  std::atomic<bool> stop{false};
  const uint64_t start_ns = lab::now_ns();
  const uint64_t end_ns = start_ns + args.duration_s * 1'000'000'000ull;

  std::thread rx_thread([&] {
    lab::pin_this_thread(args.rx_cpu);
    receiver.run(stop);
  });

  std::vector<std::thread> engine_threads;
  for (int i = 0; i < args.engines; ++i) {
    engine_threads.emplace_back([&, i] {
      if (args.engine_cpu0 >= 0) lab::pin_this_thread(args.engine_cpu0 + i);
      engines[i]->run(stop, end_ns);
    });
  }

  for (std::thread& t : engine_threads) t.join();
  stop.store(true);
  rx_thread.join();

  // Merge per-engine histograms for the report.
  lab::LatencyHistogram wire_to_engine, match_time;
  uint64_t processed = 0, trades = 0, open_orders = 0;
  for (const auto& e : engines) {
    wire_to_engine.merge(e->wire_to_engine);
    match_time.merge(e->match_time);
    processed += e->processed;
    trades += e->trades.count;
    open_orders += e->open_orders;
  }

  const lab::StageStats stages[] = {
      {"wire->engine", &wire_to_engine},
      {"engine (match)", &match_time},
  };
  lab::print_report(stdout, lab::now_ns() - start_ns, processed, trades,
                    stages, 2);

  const auto& st = receiver.stats();
  printf("receiver: mode=%s packets=%lu messages=%lu gaps=%lu ring_full=%lu "
         "syscalls=%lu\n",
         lab::FeedReceiver<lab::ShardedSink>::mode_name(args.mode),
         static_cast<unsigned long>(st.packets),
         static_cast<unsigned long>(st.messages),
         static_cast<unsigned long>(st.gaps),
         static_cast<unsigned long>(st.ring_full),
         static_cast<unsigned long>(st.syscalls));
  printf("engines: %d  open_orders=%lu\n", args.engines,
         static_cast<unsigned long>(open_orders));
  return 0;
}
