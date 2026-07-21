# Architecture Notes

## Threading model

One receiver thread and N engine threads, connected by one SPSC ring per
engine:

- **Receiver thread**: owns the UDP socket (epoll, busy-poll, recvmmsg, or
  io_uring multishot — see docs/benchmarks/receive_modes.md). Drains every
  ready datagram, reinterprets the payload as `wire::Msg` records (fixed
  40-byte layout, naturally aligned, host byte order), and routes each
  message by `symbol % engines` into that engine's ring. No heap allocation,
  no locks.
- **Engine threads**: each pops from its own ring, updates its shard's
  per-symbol order books through matchers, records two histograms
  (wire→engine latency from the embedded send timestamp, and matcher
  processing time). Histograms are merged after the run.

Single-writer principle throughout: each data structure has exactly one
mutating thread — the receiver is the sole producer of every ring, each
engine the sole consumer of its ring and sole owner of its books — so the
only synchronization points in the whole pipeline are the rings'
acquire/release pairs.

## Why the book layout looks like this

- **Flat vector of level pointers, best at back**: top-of-book operations
  dominate real flow. `back()` is one cache line; inserts/erases near the top
  shift only a handful of pointers. `std::map` pays pointer-chasing and an
  allocation per node — `book_bench` quantifies the gap.
- **Intrusive FIFO per level**: orders embed their own links, so queue
  operations are pointer swaps with no container overhead, and an order can
  unlink itself in O(1) given just its pointer.
- **`FlatHashMap<OrderId, Order*>`**: open addressing + linear probing +
  backward-shift deletion. One contiguous array, one or two cache lines per
  lookup. Key 0 is the empty sentinel (order ids start at 1).
- **`ObjectPool` slabs**: orders and levels are recycled through intrusive
  free lists. After warmup the hot path performs zero allocations
  (asserted by `PoolReuseNoGrowthAfterWarmup`).

## Wire format

`wire::Msg` is 40 bytes, fields ordered largest-first so natural alignment
holds without `#pragma pack` (packed structs generate unaligned loads). One
datagram carries up to 32 messages. `send_ts_ns` (CLOCK_MONOTONIC at send) is
the basis for end-to-end latency; sender and receiver are assumed to share a
clock (same host or PTP-synced).

A `static_assert(sizeof(Msg) == 40)` pins the layout; any accidental change
breaks the build rather than silently corrupting the stream.

## Measurement methodology

- Histograms are HDR-style log-linear (64 exponent buckets × 32 linear
  sub-buckets, ~3% bounded relative error, flat 16 KiB). `record()` is a few
  arithmetic ops and one increment — cheap enough for per-message use.
- `TscClock` calibrates RDTSC against CLOCK_MONOTONIC for cycle-accurate
  interval timing where syscall overhead would distort the measurement.
- Reports print p50 / p99 / p99.9 / max per stage: tail latencies, not
  averages, are the number that matters.

## Determinism as a testing strategy

The matcher is deliberately single-threaded per symbol. Replay tests run the
same seeded flow twice and require byte-identical trade tapes; differential
tests drive the fast book and a naive `std::map` reference with identical
operation streams and compare externally visible state after every step.
