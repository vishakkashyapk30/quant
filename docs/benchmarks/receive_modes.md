# Receive-Mode Comparison: epoll vs busy-poll vs recvmmsg vs io_uring

## Setup

- Host: 16-core x86-64 Linux 6.14, loopback UDP, single receiver thread
  pinned to an isolated core (`--rx-cpu 2`), one engine thread (`--engine-cpu0 4`).
- Load: `exchange_sim 127.0.0.1 9100 500000 9 42 4` — 4.5M messages at
  500K msg/s across 4 symbols, up to 32 messages per datagram.
- Measurement: `wire->engine` = send-side `CLOCK_MONOTONIC` timestamp to
  engine-thread ring pop, HDR histogram, one 9-second run per mode.
- Command: `latency_lab --port 9100 --mode <mode> --engines 1 ...`

## Results

| Mode | mean (ns) | p50 (ns) | p99 (ns) | receive syscalls | notes |
|---|---|---|---|---|---|
| `epoll` | 18,334 | 5,376 | 26,112 | 4.25M | blocking `epoll_wait` + drain |
| `busy-poll` | **15,593** | **3,776** | **21,504** | 18.9M | nonblocking `recv` spin |
| `recvmmsg` | 16,155 | 4,224 | 26,624 | 14.4M | spin + batched dequeue |
| `io_uring` | 16,631 | 4,352 | 35,840 | 22.3M | multishot recv + buffer ring |

All modes: 4.5M messages, zero sequence gaps, zero ring-full drops. The
p99.9 (~3.7 ms) is identical across modes — it is sender-side batching bursts
(the simulator paces in 32-message datagrams), not a receive artifact.

## Takeaways

1. **Busy-polling wins on latency, as expected.** Removing the
   `epoll_wait` wakeup (syscall + scheduler hop) cuts p50 by ~30%
   (5.4 us -> 3.8 us). The cost is a core pinned at 100% — the standard
   HFT trade.
2. **recvmmsg is about syscall economics, not tail latency.** It performs
   like busy-poll (same spin loop) while doing ~25% fewer syscalls by
   dequeuing up to 32 datagrams per call. Its advantage grows when
   datagrams are small and per-packet (rather than per-batch): with 1
   message per datagram the syscall reduction approaches 32x.
3. **io_uring is competitive but not a free win on plain UDP.** With a
   naive implementation it was catastrophically *slow* (p50 ~130 us) —
   see the pitfalls below. Correctly configured it matches recvmmsg, and
   its real advantages (batched multi-fd submission, zero-syscall SQPOLL
   mode, registered buffers) don't show on a single busy-polled socket.

## io_uring pitfalls found while building this

Documented because each one was worth a debugging session:

- **Deferred task-work vs userspace polling.** With default setup flags,
  UDP receive completions are delivered via kernel task-work that runs on
  kernel entry. A pure userspace `io_uring_peek_cqe()` loop never enters
  the kernel, so completions sat unposted until some unrelated syscall
  flushed them — measured as ~170 us of added p50. Fix:
  `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` plus an
  explicit `io_uring_get_events()` when the CQ is empty.
- **Parallel recv SQEs reorder datagrams.** A pipeline of N independent
  `recv` requests on one socket completes out of order under load; the
  sequence checker flagged gaps on an otherwise lossless loopback path.
  Fix: a single **multishot** recv (`io_uring_prep_recv_multishot`) with a
  provided **buffer ring** (`io_uring_setup_buf_ring`) — one in-flight
  request, kernel picks a buffer per datagram, order preserved, and the
  request re-arms itself until buffer exhaustion (`IORING_CQE_F_MORE`).

## Reproducing

```bash
# terminal 1
./build/latency_lab --port 9100 --mode busy-poll --duration 11 --engines 1 \
    --rx-cpu 2 --engine-cpu0 4
# terminal 2
./build/exchange_sim 127.0.0.1 9100 500000 9 42 4
```
