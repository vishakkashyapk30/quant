#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "common/wire.hpp"

namespace lab {

// Thin UDP sender that batches wire::Msg records into datagrams.
// Scaffolding by design — the interesting latency work is on the receive
// side (see feed_handler/).
class UdpSender {
 public:
  UdpSender(const std::string& host, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket() failed");
    std::memset(&dest_, 0, sizeof(dest_));
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &dest_.sin_addr) != 1) {
      throw std::runtime_error("bad address: " + host);
    }
  }

  ~UdpSender() {
    if (fd_ >= 0) ::close(fd_);
  }

  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  void send_batch(const wire::Msg* msgs, size_t n) {
    ::sendto(fd_, msgs, n * sizeof(wire::Msg), 0,
             reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
  }

 private:
  int fd_ = -1;
  sockaddr_in dest_{};
};

}  // namespace lab
