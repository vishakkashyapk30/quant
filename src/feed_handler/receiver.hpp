#pragma once

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <stdexcept>

#include "common/spsc_ring.hpp"
#include "common/timing.hpp"
#include "common/wire.hpp"

#if defined(LAB_HAVE_LIBURING)
#include <liburing.h>
#endif

namespace lab {

// UDP feed receiver with four receive strategies for latency comparison:
//
//   kEpoll    - blocking epoll_wait per wakeup; kind to the CPU, pays a
//               syscall + scheduler wakeup on every quiet->busy transition.
//   kBusyPoll - nonblocking recv in a spin loop; burns a core but removes
//               wakeup latency entirely. This is the standard HFT trade.
//   kRecvmmsg - busy-poll spin, but drains bursts with one recvmmsg syscall
//               for up to kMmsgBatch datagrams instead of one recv each:
//               same wakeup profile, ~1/N the syscall count under load.
//   kIoUring  - kernel completion queue with a pre-registered pipeline of
//               receive buffers; the hot loop peeks CQEs (no syscall when
//               completions are already posted) and only enters the kernel
//               to resubmit consumed buffers.
//
// Parsing is zero-copy in all modes: datagrams land in pre-allocated
// buffers and are reinterpret_cast as wire::Msg records (fixed-size,
// naturally aligned), then handed by value to the Sink. No heap allocation
// per packet.
//
// Sink is any callable `bool(const wire::Msg&)` returning false when the
// message was dropped (e.g. ring full). The single-engine sink pushes into
// one SPSC ring; the sharded sink routes by symbol to one ring per engine —
// the receiver stays the single producer of every ring either way.
template <typename Sink>
class FeedReceiver {
 public:
  enum class Mode { kEpoll, kBusyPoll, kRecvmmsg, kIoUring };

  static constexpr unsigned kMmsgBatch = 32;

  struct Stats {
    uint64_t packets = 0;
    uint64_t messages = 0;
    uint64_t gaps = 0;        // sequence discontinuities observed
    uint64_t ring_full = 0;   // messages dropped because consumer lagged
    uint64_t syscalls = 0;    // receive-path syscalls (recv/recvmmsg/submit)
  };

  static const char* mode_name(Mode m) {
    switch (m) {
      case Mode::kEpoll: return "epoll";
      case Mode::kBusyPoll: return "busy-poll";
      case Mode::kRecvmmsg: return "recvmmsg";
      case Mode::kIoUring: return "io_uring";
    }
    return "?";
  }

  static constexpr bool io_uring_available() {
#if defined(LAB_HAVE_LIBURING)
    return true;
#else
    return false;
#endif
  }

  FeedReceiver(uint16_t port, Mode mode, Sink& out) : mode_(mode), out_(out) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket() failed");
    const int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Large receive buffer so bursts survive consumer hiccups.
    const int rcvbuf = 8 * 1024 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("bind() failed");
    }
  }

  ~FeedReceiver() {
    if (epfd_ >= 0) ::close(epfd_);
    if (fd_ >= 0) ::close(fd_);
  }

  FeedReceiver(const FeedReceiver&) = delete;
  FeedReceiver& operator=(const FeedReceiver&) = delete;

  uint16_t bound_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
  }

  // Runs until stop() is called from another thread.
  void run(std::atomic<bool>& stop_flag) {
    switch (mode_) {
      case Mode::kEpoll: run_epoll(stop_flag); break;
      case Mode::kBusyPoll: run_busy_poll(stop_flag); break;
      case Mode::kRecvmmsg: run_recvmmsg(stop_flag); break;
      case Mode::kIoUring: run_io_uring(stop_flag); break;
    }
  }

  const Stats& stats() const { return stats_; }

 private:
  void run_epoll(std::atomic<bool>& stop_flag) {
    epfd_ = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd_;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd_, &ev);
    epoll_event events[16];
    while (!stop_flag.load(std::memory_order_relaxed)) {
      const int n = ::epoll_wait(epfd_, events, 16, /*timeout_ms=*/100);
      ++stats_.syscalls;
      if (n > 0) drain_recv();
    }
  }

  void run_busy_poll(std::atomic<bool>& stop_flag) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
      if (!drain_recv()) cpu_pause();
    }
  }

  void run_recvmmsg(std::atomic<bool>& stop_flag) {
    alignas(8) static thread_local char bufs[kMmsgBatch][wire::kMaxPacketBytes];
    mmsghdr hdrs[kMmsgBatch];
    iovec iovs[kMmsgBatch];
    for (unsigned i = 0; i < kMmsgBatch; ++i) {
      iovs[i] = {bufs[i], sizeof(bufs[i])};
      std::memset(&hdrs[i], 0, sizeof(hdrs[i]));
      hdrs[i].msg_hdr.msg_iov = &iovs[i];
      hdrs[i].msg_hdr.msg_iovlen = 1;
    }
    while (!stop_flag.load(std::memory_order_relaxed)) {
      const int n = ::recvmmsg(fd_, hdrs, kMmsgBatch, MSG_DONTWAIT, nullptr);
      ++stats_.syscalls;
      if (n <= 0) {
        cpu_pause();
        continue;
      }
      for (int i = 0; i < n; ++i) {
        consume_packet(bufs[i], hdrs[i].msg_len);
      }
    }
  }

  // Multishot recv with a provided buffer ring: ONE request serves every
  // datagram, so arrival order is preserved (N parallel recv SQEs would
  // complete out of order and read as sequence gaps). The kernel picks a
  // buffer from the registered ring per datagram; we hand buffers back after
  // parsing. DEFER_TASKRUN makes completion task-work run only when we ask
  // (get_events/submit) — without it a pure userspace peek loop never
  // triggers task-work and completions sit unposted (~170 us added latency
  // measured on loopback).
  void run_io_uring(std::atomic<bool>& stop_flag) {
#if defined(LAB_HAVE_LIBURING)
    constexpr unsigned kBufs = 256;  // power of two
    io_uring ring;
    if (io_uring_queue_init(
            256, &ring,
            IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN) != 0) {
      if (io_uring_queue_init(256, &ring, 0) != 0) {
        throw std::runtime_error("io_uring_queue_init failed");
      }
    }

    alignas(8) static thread_local char bufs[kBufs][wire::kMaxPacketBytes];
    int rc = 0;
    io_uring_buf_ring* br =
        io_uring_setup_buf_ring(&ring, kBufs, /*bgid=*/0, 0, &rc);
    if (br == nullptr) {
      io_uring_queue_exit(&ring);
      throw std::runtime_error("io_uring_setup_buf_ring failed");
    }
    for (unsigned i = 0; i < kBufs; ++i) {
      io_uring_buf_ring_add(br, bufs[i], wire::kMaxPacketBytes, i,
                            io_uring_buf_ring_mask(kBufs), static_cast<int>(i));
    }
    io_uring_buf_ring_advance(br, kBufs);

    auto arm_multishot = [&] {
      io_uring_sqe* sqe = io_uring_get_sqe(&ring);
      io_uring_prep_recv_multishot(sqe, fd_, nullptr, 0, 0);
      sqe->buf_group = 0;
      sqe->flags |= IOSQE_BUFFER_SELECT;
      io_uring_submit(&ring);
      ++stats_.syscalls;
    };
    arm_multishot();

    while (!stop_flag.load(std::memory_order_relaxed)) {
      io_uring_cqe* cqe = nullptr;
      // Peek already-posted completions without entering the kernel.
      if (io_uring_peek_cqe(&ring, &cqe) != 0) {
        io_uring_get_events(&ring);  // run deferred task-work
        ++stats_.syscalls;
        cpu_pause();
        continue;
      }
      const int res = cqe->res;
      const unsigned flags = cqe->flags;
      if (res > 0 && (flags & IORING_CQE_F_BUFFER)) {
        const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
        consume_packet(bufs[bid], static_cast<size_t>(res));
        io_uring_buf_ring_add(br, bufs[bid], wire::kMaxPacketBytes,
                              static_cast<unsigned short>(bid),
                              io_uring_buf_ring_mask(kBufs), 0);
        io_uring_buf_ring_advance(br, 1);
      }
      io_uring_cqe_seen(&ring, cqe);
      // Re-arm when the multishot terminates (buffer exhaustion, error).
      if (!(flags & IORING_CQE_F_MORE)) arm_multishot();
    }
    io_uring_free_buf_ring(&ring, br, kBufs, 0);
    io_uring_queue_exit(&ring);
#else
    (void)stop_flag;
    throw std::runtime_error("built without liburing; io_uring mode unavailable");
#endif
  }

  // Receives every datagram currently queued via plain recv; returns true if
  // any arrived. Shared by epoll and busy-poll modes.
  bool drain_recv() {
    alignas(8) char buf[wire::kMaxPacketBytes];
    bool got_any = false;
    while (true) {
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
      ++stats_.syscalls;
      if (n <= 0) break;
      got_any = true;
      consume_packet(buf, static_cast<size_t>(n));
    }
    return got_any;
  }

  void consume_packet(const char* buf, size_t bytes) {
    ++stats_.packets;
    const size_t count = bytes / sizeof(wire::Msg);
    const auto* msgs = reinterpret_cast<const wire::Msg*>(buf);
    for (size_t i = 0; i < count; ++i) {
      if (expected_seq_ != 0 && msgs[i].seq != expected_seq_) {
        ++stats_.gaps;
      }
      expected_seq_ = msgs[i].seq + 1;
      ++stats_.messages;
      if (!out_(msgs[i])) {
        ++stats_.ring_full;
      }
    }
  }

  int fd_ = -1;
  int epfd_ = -1;
  Mode mode_;
  Sink& out_;
  Stats stats_;
  uint32_t expected_seq_ = 0;
};

// Sink pushing every message into a single SPSC ring.
struct RingSink {
  SpscRing<wire::Msg>& ring;
  bool operator()(const wire::Msg& m) { return ring.try_push(m); }
};

// Sink sharding messages across one ring per engine thread by symbol.
// Symbols are dense small integers, so modulo distributes evenly; the
// receiver remains the sole producer of every ring (SPSC holds).
struct ShardedSink {
  SpscRing<wire::Msg>* const* rings;
  size_t n;
  bool operator()(const wire::Msg& m) {
    return rings[m.symbol % n]->try_push(m);
  }
};

}  // namespace lab
