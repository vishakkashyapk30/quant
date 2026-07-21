# Order Book Cache Analysis: flat/intrusive vs std::map

## Wall-clock (Release, -O3 -march=native, GCC 14)

`book_bench 2000000 100` — identical random add/cancel/modify stream,
~100 price levels per side, ~55/30/15 op mix, 3 interleaved rounds:

| Book | ns/op | throughput |
|---|---|---|
| flat `OrderBook` (sorted level vector + intrusive FIFOs + pools) | **~99** | ~10.1M ops/s |
| `std::map` `ReferenceBook` (node containers, per-node allocation) | ~743 | ~1.35M ops/s |

**7.5x** on the same workload.

## Cache simulation (valgrind --tool=cachegrind)

`book_bench 100000 100 <flat|map>` — 300K ops total (3 rounds), per-book
runs so counts attribute cleanly. Absolute counts below; the per-op columns
are divided by 300K.

| Metric | flat | std::map | ratio |
|---|---|---|---|
| Instruction refs | 84.8M | 202.5M | 2.4x |
| Data refs | 23.8M | 73.6M | **3.1x** |
| Data refs / op | 79 | 245 | 3.1x |
| D1 misses | 1.64M | 4.71M | **2.9x** |
| D1 misses / op | 5.5 | 15.7 | 2.9x |
| LLd misses | 213K | 283K | 1.3x |

## Why the flat book wins

- **One pointer dereference to top-of-book.** Best level is `vector::back()`;
  `std::map` walks tree nodes scattered across the heap, and every hop is a
  potential D1 miss (visible in the 9.0% read-miss rate for the map book vs
  6.1% for the flat book, on 3x the reference volume).
- **No allocation on the hot path.** Orders and levels come from slab pools
  (recycled LIFO, so recently-freed = cache-warm); the map book calls the
  allocator on every insert/erase, which is both instructions (2.4x I refs)
  and cache pollution.
- **Intrusive links.** Unlinking an order is two pointer writes inside a
  struct that is already in cache from the hash-map lookup; `std::deque`
  erase shuffles elements.
- **Activity clusters near the top.** Level insert/erase in the sorted
  vector shifts a few contiguous pointers — hardware prefetcher territory —
  instead of rebalancing a tree.

Note cachegrind's simulated D1 *miss rate* is similar for both books (~6-7%);
the difference is that the map book issues 3.1x more data references to do
the same work, so it eats ~2.9x more absolute misses per operation.

## Huge pages (pool slab backing)

`ObjectPool` can back slabs with 2 MiB pages (`--huge-pages`), falling back
MAP_HUGETLB -> transparent huge pages -> normal pages. On this host (THP
`madvise` mode, no explicit hugepage reservation) the effect at 500K msg/s
with ~450K resting orders (~22 MiB of pool) was a modest tail improvement in
match latency — p99.9 1,216 -> 1,056 ns — and no p50 change. Expected: the
book's working set per message is a handful of lines, so dTLB pressure only
shows at the tail. Larger books (multi-symbol, deeper pools) and explicit
`vm.nr_hugepages` reservation should widen the gap.

## Multi-symbol sharding

`--engines 4` with 8 symbols at 1M msg/s (loopback, one receiver thread
routing by `symbol % engines` into per-engine SPSC rings):

```
messages: 8999999   trades: 1569672   gaps: 0   ring_full: 0
wire->engine   p50 4,032 ns   p99 7,040 ns
engine (match) p50   184 ns   p99   704 ns
```

9M messages, zero loss, zero gaps: the single-producer/single-consumer
property holds per ring, so sharding needs no locks anywhere.

## Reproducing

```bash
./build/book_bench 2000000 100
valgrind --tool=cachegrind ./build/book_bench 100000 100 flat
valgrind --tool=cachegrind ./build/book_bench 100000 100 map
```
