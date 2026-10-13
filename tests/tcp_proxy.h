// Tiny TCP proxy for fault injection. In BLACKHOLE mode every byte is silently discarded and the
// connections stay open, which is what a vanished pod IP looks like to a client: no RST, no FIN,
// just silence. Connections created while black-holed (or alive when it started) stay stuck even
// after the proxy returns to FORWARD; only NEW connections work again.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace kestrel::testing_util {

class TcpProxy {
 public:
  explicit TcpProxy(int upstream_port) : upstream_port_(upstream_port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(listen_fd_, 64);
    socklen_t len = sizeof a;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    acceptor_ = std::thread([this] { AcceptLoop(); });
  }
  ~TcpProxy() {
    stop_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& t : conns_) t.join();
    }
    ::close(listen_fd_);
  }

  int port() const { return port_; }
  void SetBlackhole(bool on) { blackhole_.store(on); }

 private:
  void AcceptLoop() {
    while (!stop_.load()) {
      pollfd p{listen_fd_, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) continue;
      const bool stuck = blackhole_.load();
      std::lock_guard<std::mutex> lk(mu_);
      conns_.emplace_back([this, client, stuck] { Pump(client, stuck); });
    }
  }

  void Pump(int client, bool born_stuck) {
    int upstream = -1;
    if (!born_stuck) {
      upstream = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(static_cast<uint16_t>(upstream_port_));
      ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
      if (::connect(upstream, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(upstream);
        upstream = -1;  // upstream down: behave like a black hole too
      }
    }
    bool stuck = born_stuck || upstream < 0;
    char buf[16384];
    while (!stop_.load()) {
      pollfd p[2] = {{client, POLLIN, 0}, {upstream, POLLIN, 0}};
      if (::poll(p, upstream >= 0 ? 2 : 1, 50) <= 0) continue;
      if (blackhole_.load()) stuck = true;
      if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        ssize_t n = ::recv(client, buf, sizeof buf, 0);
        if (n <= 0) break;
        if (!stuck && upstream >= 0) ::send(upstream, buf, static_cast<size_t>(n), MSG_NOSIGNAL);
      }
      if (upstream >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
        ssize_t n = ::recv(upstream, buf, sizeof buf, 0);
        if (n <= 0) {
          if (!stuck) break;  // a normal close propagates; a stuck connection stays silent
          ::close(upstream);  // the upstream really went away; keep the client side hanging
          upstream = -1;
          continue;
        }
        if (!stuck) ::send(client, buf, static_cast<size_t>(n), MSG_NOSIGNAL);
      }
    }
    ::close(client);
    if (upstream >= 0) ::close(upstream);
  }

  int upstream_port_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<bool> blackhole_{false};
  std::thread acceptor_;
  std::mutex mu_;
  std::vector<std::thread> conns_;
};

}  // namespace kestrel::testing_util
