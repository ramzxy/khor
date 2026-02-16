#include "osc/osc.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace khor {
namespace {

static void pad4(std::vector<uint8_t>& b) {
  while ((b.size() & 3u) != 0) b.push_back(0);
}

static void put_str(std::vector<uint8_t>& b, const char* s) {
  if (!s) s = "";
  const std::size_t n = std::strlen(s);
  b.insert(b.end(), (const uint8_t*)s, (const uint8_t*)s + n);
  b.push_back(0);
  pad4(b);
}

static void put_i32(std::vector<uint8_t>& b, int32_t v) {
  uint32_t be = htonl((uint32_t)v);
  const uint8_t* p = (const uint8_t*)&be;
  b.insert(b.end(), p, p + 4);
}

static void put_f32(std::vector<uint8_t>& b, float f) {
  uint32_t u = 0;
  static_assert(sizeof(float) == sizeof(uint32_t));
  std::memcpy(&u, &f, sizeof(u));
  u = htonl(u);
  const uint8_t* p = (const uint8_t*)&u;
  b.insert(b.end(), p, p + 4);
}

static std::vector<uint8_t> osc_msg_note(const NoteEvent& ev) {
  std::vector<uint8_t> b;
  b.reserve(64);
  put_str(b, "/khor/note");
  put_str(b, ",iff");
  put_i32(b, (int32_t)std::clamp(ev.midi, 0, 127));
  put_f32(b, std::clamp(ev.velocity, 0.0f, 1.0f));
  put_f32(b, std::max(0.0f, ev.dur_s));
  return b;
}

static std::vector<uint8_t> osc_msg_signal(const char* name, float v01) {
  std::vector<uint8_t> b;
  b.reserve(96);
  put_str(b, "/khor/signal");
  put_str(b, ",sf");
  put_str(b, name ? name : "");
  put_f32(b, std::clamp(v01, 0.0f, 1.0f));
  return b;
}

static std::vector<uint8_t> osc_msg_metrics(const SignalRates& r) {
  std::vector<uint8_t> b;
  b.reserve(128);
  put_str(b, "/khor/metrics");
  put_str(b, ",ffffff");
  put_f32(b, (float)r.exec_s);
  put_f32(b, (float)r.rx_kbs);
  put_f32(b, (float)r.tx_kbs);
  put_f32(b, (float)r.csw_s);
  put_f32(b, (float)r.blk_r_kbs);
  put_f32(b, (float)r.blk_w_kbs);
  return b;
}

} // namespace

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
  const auto payload = osc_msg_note(ev);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

void OscClient::send_signal(const char* name, float value01) {
  if (!is_running()) return;
  const auto payload = osc_msg_signal(name, value01);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

void OscClient::send_metrics(const SignalRates& r) {
  if (!is_running()) return;
  const auto payload = osc_msg_metrics(r);
  (void)::sendto(impl_->fd, payload.data(), payload.size(), MSG_DONTWAIT, (const sockaddr*)&impl_->addr, impl_->addr_len);
}

} // namespace khor
