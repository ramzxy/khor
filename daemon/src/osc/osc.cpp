#include "osc/osc.h"

#include <cstring>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "osc/encode.h"

namespace khor {
struct OscClient::Impl {
  int fd = -1;
  sockaddr_storage addr{};
  socklen_t addr_len = 0;
  std::string host;
  int port = 0;
};

OscClient::OscClient() : impl_(new Impl()) {}
OscClient::~OscClient() { stop(); delete impl_; impl_ = nullptr; }

bool OscClient::start(const std::string& host, int port, std::string* err) {
  if (!impl_) return false;
  stop();

  if (port < 1 || port > 65535) {
    if (err) *err = "invalid OSC port";
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
    if (err) *err = "failed to resolve OSC host";
    return false;
  }

  int fd = -1;
  sockaddr_storage out_addr{};
  socklen_t out_len = 0;
  for (addrinfo* it = res; it; it = it->ai_next) {
    fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) continue;
    std::memcpy(&out_addr, it->ai_addr, it->ai_addrlen);
    out_len = (socklen_t)it->ai_addrlen;
    break;
  }
  freeaddrinfo(res);

  if (fd < 0) {
    if (err) *err = "failed to create OSC UDP socket";
    return false;
  }

  impl_->fd = fd;
  impl_->addr = out_addr;
  impl_->addr_len = out_len;
  impl_->host = host;
  impl_->port = port;
  return true;
}

void OscClient::stop() {
  if (!impl_) return;
  if (impl_->fd >= 0) ::close(impl_->fd);
  impl_->fd = -1;
  impl_->addr_len = 0;
  impl_->host.clear();
  impl_->port = 0;
}

bool OscClient::is_running() const { return impl_ && impl_->fd >= 0; }

void OscClient::send_note(const NoteEvent& ev) {
  if (!is_running()) return;
  const auto payload = osc::encode_note(ev);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

void OscClient::send_signal(const char* name, float value01) {
  if (!is_running()) return;
  const auto payload = osc::encode_signal(name, value01);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

void OscClient::send_metrics(const SignalRates& r) {
  if (!is_running()) return;
  const auto payload = osc::encode_metrics(r);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

} // namespace khor
