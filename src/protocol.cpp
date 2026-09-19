// Congestion Recovery - framed transport implementation (Winsock / BSD sockets).
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/protocol.hpp"

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "congestion_recovery/journal.hpp"

namespace congestion_recovery {
namespace {

constexpr std::uint32_t kFrameMagic = 0x31465243u;  // "CRF1"
constexpr std::size_t kFramePrefixBytes = 8u;       // magic + total_len
constexpr std::size_t kFrameHeaderBytes = 1u + 8u + 8u + 8u + 8u + 4u;
constexpr std::size_t kFrameTrailerBytes = 4u;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;
#endif

void close_native(NativeSocket s) noexcept {
  if (s == kInvalidNative) {
    return;
  }
#ifdef _WIN32
  (void)::closesocket(s);
#else
  (void)::close(s);
#endif
}

std::string last_socket_error() {
#ifdef _WIN32
  const int code = ::WSAGetLastError();
  switch (code) {
    case WSAETIMEDOUT: return "timeout";
    case WSAECONNRESET: return "connection reset";
    case WSAECONNABORTED: return "connection aborted";
    case WSAENOTCONN: return "not connected";
    default: break;
  }
  return "socket error " + std::to_string(code);
#else
  return std::string(std::strerror(errno));
#endif
}

bool would_block() noexcept {
#ifdef _WIN32
  const int code = ::WSAGetLastError();
  return code == WSAETIMEDOUT;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Bounded writer / reader
// ---------------------------------------------------------------------------
bool Writer::ensure(std::size_t extra) noexcept {
  if (bad_) {
    return false;
  }
  if (extra > kMaxBodyBytes || buf_.size() > kMaxBodyBytes - extra) {
    bad_ = true;
    return false;
  }
  return true;
}

void Writer::put_u8(std::uint8_t v) {
  if (!ensure(1u)) {
    return;
  }
  buf_.push_back(v);
}

void Writer::put_u16(std::uint16_t v) {
  if (!ensure(2u)) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
  }
}

void Writer::put_u32(std::uint32_t v) {
  if (!ensure(4u)) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
  }
}

void Writer::put_u64(std::uint64_t v) {
  if (!ensure(8u)) {
    return;
  }
  for (int i = 0; i < 8; ++i) {
    buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
  }
}

void Writer::put_i64(std::int64_t v) { put_u64(static_cast<std::uint64_t>(v)); }

void Writer::put_bool(bool v) { put_u8(v ? 1u : 0u); }

void Writer::put_f64(double v) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
  std::memcpy(&bits, &v, sizeof(bits));
  put_u64(bits);
}

void Writer::put_str(const std::string& v) {
  if (v.size() > kMaxStringBytes) {
    bad_ = true;
    return;
  }
  put_u32(static_cast<std::uint32_t>(v.size()));
  if (!ensure(v.size())) {
    return;
  }
  buf_.insert(buf_.end(), v.begin(), v.end());
}

void Writer::put_bytes(const std::vector<std::uint8_t>& v) {
  if (v.size() > kMaxBodyBytes) {
    bad_ = true;
    return;
  }
  put_u32(static_cast<std::uint32_t>(v.size()));
  if (!ensure(v.size())) {
    return;
  }
  buf_.insert(buf_.end(), v.begin(), v.end());
}

bool Reader::need(std::size_t n) noexcept {
  if (bad_ || n > size_ - off_) {
    bad_ = true;
    return false;
  }
  return true;
}

std::uint8_t Reader::get_u8() {
  if (!need(1u)) {
    return 0;
  }
  return data_[off_++];
}

std::uint16_t Reader::get_u16() {
  if (!need(2u)) {
    return 0;
  }
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i) {
    v |= static_cast<std::uint16_t>(data_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  off_ += 2u;
  return v;
}

std::uint32_t Reader::get_u32() {
  if (!need(4u)) {
    return 0;
  }
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(data_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  off_ += 4u;
  return v;
}

std::uint64_t Reader::get_u64() {
  if (!need(8u)) {
    return 0;
  }
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(data_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  off_ += 8u;
  return v;
}

std::int64_t Reader::get_i64() { return static_cast<std::int64_t>(get_u64()); }

bool Reader::get_bool() { return get_u8() != 0u; }

double Reader::get_f64() {
  const std::uint64_t bits = get_u64();
  double v = 0.0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

std::string Reader::get_str() {
  const std::uint32_t size = get_u32();
  if (bad_ || size > kMaxStringBytes || !need(size)) {
    bad_ = true;
    return {};
  }
  std::string v(reinterpret_cast<const char*>(data_ + off_), size);
  off_ += size;
  return v;
}

std::vector<std::uint8_t> Reader::get_bytes() {
  const std::uint32_t size = get_u32();
  if (bad_ || size > kMaxBodyBytes || !need(size)) {
    bad_ = true;
    return {};
  }
  std::vector<std::uint8_t> v(data_ + off_, data_ + off_ + size);
  off_ += size;
  return v;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
bool encode_frame(const Frame& frame, std::vector<std::uint8_t>& out, std::string& err) {
  err.clear();
  if (!is_valid(frame.kind)) {
    err = "invalid message kind";
    return false;
  }
  if (frame.body.size() > kMaxBodyBytes) {
    err = "frame body exceeds the maximum size";
    return false;
  }
  Writer body;
  body.put_u8(static_cast<std::uint8_t>(frame.kind));
  body.put_u64(frame.epoch.value());
  body.put_u64(frame.worker.value());
  body.put_u64(frame.boot.value());
  body.put_u64(frame.sequence);
  body.put_u32(static_cast<std::uint32_t>(frame.body.size()));
  if (body.bad()) {
    err = "frame header could not be encoded";
    return false;
  }
  std::vector<std::uint8_t> header = body.buffer();
  header.insert(header.end(), frame.body.begin(), frame.body.end());

  const std::uint32_t total = static_cast<std::uint32_t>(header.size() + kFrameTrailerBytes);
  out.clear();
  out.reserve(kFramePrefixBytes + total);
  Writer prefix;
  prefix.put_u32(kFrameMagic);
  prefix.put_u32(total);
  out.insert(out.end(), prefix.buffer().begin(), prefix.buffer().end());
  out.insert(out.end(), header.begin(), header.end());
  const std::uint32_t checksum = crc32(out.data() + kFramePrefixBytes, header.size());
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((checksum >> (8 * i)) & 0xffu));
  }
  return true;
}

bool decode_frame(const uint8_t* data, std::size_t size, Frame& out, std::size_t& consumed,
                  std::string& err) {
  err.clear();
  consumed = 0;
  out = Frame{};
  if (data == nullptr) {
    err = "malformed";
    return false;
  }
  if (size < kFramePrefixBytes) {
    err = "truncated";
    return false;
  }
  Reader prefix(data, kFramePrefixBytes);
  const std::uint32_t magic = prefix.get_u32();
  const std::uint32_t total = prefix.get_u32();
  if (magic != kFrameMagic) {
    err = "bad magic";
    return false;
  }
  if (total < kFrameHeaderBytes + kFrameTrailerBytes) {
    err = "undersized frame";
    return false;
  }
  if (total > kMaxFrameBytes) {
    err = "oversized frame";
    return false;
  }
  const std::size_t frame_size = kFramePrefixBytes + static_cast<std::size_t>(total);
  if (size < frame_size) {
    err = "truncated";
    return false;
  }
  const std::uint8_t* header = data + kFramePrefixBytes;
  const std::size_t header_size = static_cast<std::size_t>(total) - kFrameTrailerBytes;
  std::uint32_t stored_crc = 0;
  for (int i = 0; i < 4; ++i) {
    stored_crc |= static_cast<std::uint32_t>(data[kFramePrefixBytes + header_size + static_cast<std::size_t>(i)])
                  << (8 * i);
  }
  if (crc32(header, header_size) != stored_crc) {
    err = "checksum mismatch";
    return false;
  }
  Reader in(header, header_size);
  const MessageKind kind = static_cast<MessageKind>(in.get_u8());
  out.epoch = CoordinatorEpoch(in.get_u64());
  out.worker = WorkerId(in.get_u64());
  out.boot = WorkerBoot(in.get_u64());
  out.sequence = in.get_u64();
  const std::uint32_t body_len = in.get_u32();
  if (in.bad() || !is_valid(kind)) {
    err = "malformed header";
    return false;
  }
  if (body_len != header_size - kFrameHeaderBytes) {
    err = "body length does not match the frame";
    return false;
  }
  if (body_len > 0u) {
    const std::uint8_t* body = header + kFrameHeaderBytes;
    out.body.assign(body, body + body_len);
  }
  out.kind = kind;
  consumed = frame_size;
  return true;
}

bool is_truncated(const std::string& err) noexcept { return err == "truncated"; }

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
bool socket_system_startup(std::string& err) {
#ifdef _WIN32
  static bool started = false;
  static int result = 0;
  if (!started) {
    WSADATA data{};
    result = ::WSAStartup(MAKEWORD(2, 2), &data);
    started = true;
  }
  if (result != 0) {
    err = "WSAStartup failed";
    return false;
  }
#else
  (void)err;
#endif
  return true;
}

void socket_system_shutdown() {
#ifdef _WIN32
  (void)::WSACleanup();
#endif
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() noexcept {
  if (handle_ != kInvalidSocket) {
    close_native(static_cast<NativeSocket>(handle_));
    handle_ = kInvalidSocket;
  }
  port_ = 0;
}

bool TcpListener::listen_on(const std::string& host, std::uint16_t port, std::string& err) {
  err.clear();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results) != 0) {
    err = "address resolution failed";
    return false;
  }
  bool ok = false;
  for (addrinfo* candidate = results; candidate != nullptr && !ok; candidate = candidate->ai_next) {
    const NativeSocket s = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (s == kInvalidNative) {
      continue;
    }
    const int reuse = 1;
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (::bind(s, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      close_native(s);
      continue;
    }
    if (::listen(s, 8) != 0) {
      close_native(s);
      continue;
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
      port_ = ntohs(bound.sin_port);
    }
    handle_ = static_cast<SocketHandle>(s);
    ok = true;
  }
  ::freeaddrinfo(results);
  if (!ok) {
    err = "could not bind or listen";
    return false;
  }
  return true;
}

void TcpListener::set_timeout_ms(std::uint32_t ms) noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  const DWORD value = ms;
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = static_cast<long>(ms / 1000u);
  value.tv_usec = static_cast<long>((ms % 1000u) * 1000u);
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

SocketHandle TcpListener::accept_one(std::string& err) {
  err.clear();
  if (handle_ == kInvalidSocket) {
    err = "listener is not open";
    return kInvalidSocket;
  }
  sockaddr_storage peer{};
#ifdef _WIN32
  int peer_len = sizeof(peer);
#else
  socklen_t peer_len = sizeof(peer);
#endif
  const NativeSocket s =
      ::accept(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_len);
  if (s == kInvalidNative) {
    err = last_socket_error();
    return kInvalidSocket;
  }
  return static_cast<SocketHandle>(s);
}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

void TcpStream::close() noexcept {
  if (handle_ != kInvalidSocket) {
    close_native(static_cast<NativeSocket>(handle_));
    handle_ = kInvalidSocket;
  }
}

void TcpStream::set_timeout_ms(std::uint32_t ms) noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  const DWORD value = ms;
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value));
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = static_cast<long>(ms / 1000u);
  value.tv_usec = static_cast<long>((ms % 1000u) * 1000u);
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)::setsockopt(static_cast<NativeSocket>(handle_), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

bool TcpStream::connect_to(const std::string& host, std::uint16_t port, TcpStream& out, std::string& err) {
  err.clear();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    err = "address resolution failed";
    return false;
  }
  bool ok = false;
  for (addrinfo* candidate = results; candidate != nullptr && !ok; candidate = candidate->ai_next) {
    const NativeSocket s = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (s == kInvalidNative) {
      continue;
    }
    if (::connect(s, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      close_native(s);
      continue;
    }
    const int nodelay = 1;
    (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    out.close();
    out.handle_ = static_cast<SocketHandle>(s);
    ok = true;
  }
  ::freeaddrinfo(results);
  if (!ok) {
    err = "connect failed";
    return false;
  }
  return true;
}

bool TcpStream::send_bytes(const std::uint8_t* data, std::size_t size, std::string& err) {
  err.clear();
  if (handle_ == kInvalidSocket) {
    err = "not connected";
    return false;
  }
  std::size_t sent = 0;
  while (sent < size) {
    const int chunk = static_cast<int>(size - sent > 65536u ? 65536u : size - sent);
    const int n = ::send(static_cast<NativeSocket>(handle_), reinterpret_cast<const char*>(data + sent), chunk, 0);
    if (n <= 0) {
      err = would_block() ? "timeout" : last_socket_error();
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpStream::send_frame(const Frame& frame, std::string& err) {
  std::vector<std::uint8_t> bytes;
  if (!encode_frame(frame, bytes, err)) {
    return false;
  }
  return send_bytes(bytes.data(), bytes.size(), err);
}

bool TcpStream::recv_frame(Frame& frame, std::string& err) {
  err.clear();
  if (handle_ == kInvalidSocket) {
    err = "not connected";
    return false;
  }
  std::vector<std::uint8_t> buffer;
  buffer.reserve(4096);
  std::uint32_t expected = 0;
  std::size_t consumed = 0;
  for (;;) {
    if (expected != 0u && buffer.size() >= expected) {
      std::string decode_err;
      if (!decode_frame(buffer.data(), buffer.size(), frame, consumed, decode_err)) {
        err = decode_err;
        return false;
      }
      // Bytes beyond the first frame are not expected on this protocol.
      if (consumed != buffer.size()) {
        err = "trailing bytes after frame";
        return false;
      }
      return true;
    }
    std::uint8_t chunk[4096];
    const int want = static_cast<int>(expected != 0u && expected - buffer.size() < sizeof(chunk)
                                          ? expected - buffer.size()
                                          : sizeof(chunk));
    const int n = ::recv(static_cast<NativeSocket>(handle_), reinterpret_cast<char*>(chunk), want, 0);
    if (n == 0) {
      err = "peer closed";
      return false;
    }
    if (n < 0) {
      err = would_block() ? "timeout" : last_socket_error();
      return false;
    }
    buffer.insert(buffer.end(), chunk, chunk + n);
    if (expected == 0u && buffer.size() >= kFramePrefixBytes) {
      std::uint32_t total = 0;
      for (int i = 0; i < 4; ++i) {
        total |= static_cast<std::uint32_t>(buffer[4u + static_cast<std::size_t>(i)]) << (8 * i);
      }
      if (total > kMaxFrameBytes) {
        err = "oversized frame";
        return false;
      }
      expected = static_cast<std::uint32_t>(kFramePrefixBytes + total);
    }
  }
}

bool TcpStream::peer_alive() noexcept {
  if (handle_ == kInvalidSocket) {
    return false;
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(static_cast<NativeSocket>(handle_), &read_set);
  timeval zero{};
  zero.tv_sec = 0;
  zero.tv_usec = 0;
  const int ready = ::select(0, &read_set, nullptr, nullptr, &zero);
  if (ready <= 0) {
    return ready == 0;  // no pending data is not death; an error is
  }
  char probe = 0;
  const int n = ::recv(static_cast<NativeSocket>(handle_), &probe, 1, MSG_PEEK);
  if (n == 0) {
    return false;  // orderly close
  }
  if (n < 0) {
    return would_block();
  }
  return true;
}

}  // namespace congestion_recovery
