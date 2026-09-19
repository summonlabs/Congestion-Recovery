// Congestion Recovery - framed, checksummed coordinator/worker transport.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"

namespace congestion_recovery {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxFrameBytes = 1u << 20;      // 1 MiB
inline constexpr std::size_t kMaxBodyBytes = kMaxFrameBytes - 64u;
inline constexpr std::size_t kMaxStringBytes = 4096u;
inline constexpr std::size_t kMaxBulkItems = 4096u;

enum class MessageKind : std::uint8_t {
  UNKNOWN = 0,
  HELLO = 1,
  HELLO_ACK = 2,
  FENCE = 3,
  STAGE_GRANT = 4,
  STAGE_REPORT = 5,
  EVIDENCE_PUBLISH = 6,
  EVIDENCE_ACK = 7,
  PLAN_PUSH = 8,
  COMPLETE_GRANT = 9,
  COMPLETE_REPORT = 10,
  STATE_QUERY = 11,
  STATE_REPORT = 12,
  HEARTBEAT = 13,
  ERROR_MSG = 14,
  GOODBYE = 15,
  EVIDENCE_REQUEST = 16,
  COUNT_ = 17,
};

[[nodiscard]] constexpr bool is_valid(MessageKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(MessageKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(MessageKind v) noexcept {
  switch (v) {
    case MessageKind::HELLO: return "HELLO";
    case MessageKind::HELLO_ACK: return "HELLO_ACK";
    case MessageKind::FENCE: return "FENCE";
    case MessageKind::STAGE_GRANT: return "STAGE_GRANT";
    case MessageKind::STAGE_REPORT: return "STAGE_REPORT";
    case MessageKind::EVIDENCE_PUBLISH: return "EVIDENCE_PUBLISH";
    case MessageKind::EVIDENCE_ACK: return "EVIDENCE_ACK";
    case MessageKind::PLAN_PUSH: return "PLAN_PUSH";
    case MessageKind::COMPLETE_GRANT: return "COMPLETE_GRANT";
    case MessageKind::COMPLETE_REPORT: return "COMPLETE_REPORT";
    case MessageKind::STATE_QUERY: return "STATE_QUERY";
    case MessageKind::STATE_REPORT: return "STATE_REPORT";
    case MessageKind::HEARTBEAT: return "HEARTBEAT";
    case MessageKind::ERROR_MSG: return "ERROR_MSG";
    case MessageKind::GOODBYE: return "GOODBYE";
    case MessageKind::EVIDENCE_REQUEST: return "EVIDENCE_REQUEST";
    case MessageKind::UNKNOWN:
    case MessageKind::COUNT_: break;
  }
  return "UNKNOWN";
}

// Wire error taxonomy. Every rejection a peer can observe carries one.
enum class WireError : std::uint8_t {
  NONE = 0,
  BAD_MAGIC = 1,
  BAD_VERSION = 2,
  OVERSIZED = 3,
  TRUNCATED = 4,
  CHECKSUM = 5,
  MALFORMED = 6,
  PROTOCOL_MISMATCH = 7,
  STALE_EPOCH = 8,
  STALE_BOOT = 9,
  UNKNOWN_WORKER = 10,
  FENCED = 11,
  STALE_ATTEMPT = 12,
  UNAUTHORIZED = 13,
  STALE_COMPLETION = 14,
  DUPLICATE = 15,
  BUSY = 16,
  INTERNAL = 17,
  COUNT_ = 18,
};

[[nodiscard]] constexpr bool is_valid(WireError v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(WireError::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(WireError v) noexcept {
  switch (v) {
    case WireError::NONE: return "NONE";
    case WireError::BAD_MAGIC: return "BAD_MAGIC";
    case WireError::BAD_VERSION: return "BAD_VERSION";
    case WireError::OVERSIZED: return "OVERSIZED";
    case WireError::TRUNCATED: return "TRUNCATED";
    case WireError::CHECKSUM: return "CHECKSUM";
    case WireError::MALFORMED: return "MALFORMED";
    case WireError::PROTOCOL_MISMATCH: return "PROTOCOL_MISMATCH";
    case WireError::STALE_EPOCH: return "STALE_EPOCH";
    case WireError::STALE_BOOT: return "STALE_BOOT";
    case WireError::UNKNOWN_WORKER: return "UNKNOWN_WORKER";
    case WireError::FENCED: return "FENCED";
    case WireError::STALE_ATTEMPT: return "STALE_ATTEMPT";
    case WireError::UNAUTHORIZED: return "UNAUTHORIZED";
    case WireError::STALE_COMPLETION: return "STALE_COMPLETION";
    case WireError::DUPLICATE: return "DUPLICATE";
    case WireError::BUSY: return "BUSY";
    case WireError::INTERNAL: return "INTERNAL";
    case WireError::COUNT_: break;
  }
  return "NONE";
}

struct Frame {
  MessageKind kind{MessageKind::UNKNOWN};
  CoordinatorEpoch epoch{};
  WorkerId worker{};
  WorkerBoot boot{};
  std::uint64_t sequence{0};
  std::vector<std::uint8_t> body{};
};

// Bounded little-endian writer. Every put_* fails (sets bad()) instead of
// growing past kMaxBodyBytes.
class Writer {
 public:
  void put_u8(std::uint8_t v);
  void put_u16(std::uint16_t v);
  void put_u32(std::uint32_t v);
  void put_u64(std::uint64_t v);
  void put_i64(std::int64_t v);
  void put_bool(bool v);
  void put_f64(double v);
  void put_str(const std::string& v);
  void put_bytes(const std::vector<std::uint8_t>& v);
  [[nodiscard]] bool bad() const noexcept { return bad_; }
  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buf_; }
  [[nodiscard]] std::vector<std::uint8_t>& buffer() noexcept { return buf_; }

 private:
  bool ensure(std::size_t extra) noexcept;

  std::vector<std::uint8_t> buf_{};
  bool bad_{false};
};

// Bounded reader. Every get_* marks the reader bad on truncation instead of
// reading out of range.
class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] std::uint8_t get_u8();
  [[nodiscard]] std::uint16_t get_u16();
  [[nodiscard]] std::uint32_t get_u32();
  [[nodiscard]] std::uint64_t get_u64();
  [[nodiscard]] std::int64_t get_i64();
  [[nodiscard]] bool get_bool();
  [[nodiscard]] double get_f64();
  [[nodiscard]] std::string get_str();
  [[nodiscard]] std::vector<std::uint8_t> get_bytes();
  [[nodiscard]] bool bad() const noexcept { return bad_; }
  [[nodiscard]] bool empty() const noexcept { return off_ >= size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - off_; }

 private:
  [[nodiscard]] bool need(std::size_t n) noexcept;

  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t off_{0};
  bool bad_{false};
};

// Encode a frame into a self-delimiting byte buffer:
//   magic[4] "CRF1" | total_len u32 | crc32 u32 | header | body
[[nodiscard]] bool encode_frame(const Frame& frame, std::vector<std::uint8_t>& out, std::string& err);

// Decode one frame from a prefix of data. consumed receives the number of bytes
// the frame occupied. Returns false with err set for any malformed input; a
// short-but-plausible prefix reports err == "truncated" so callers can read
// more bytes.
[[nodiscard]] bool decode_frame(const uint8_t* data, std::size_t size, Frame& out, std::size_t& consumed,
                                std::string& err);

[[nodiscard]] bool is_truncated(const std::string& err) noexcept;

// ---- sockets -------------------------------------------------------------
// intptr_t handle: -1 is invalid. No platform header leaks into this header.
using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

// One-time process-wide socket subsystem initialization.
[[nodiscard]] bool socket_system_startup(std::string& err);
void socket_system_shutdown();

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  [[nodiscard]] bool listen_on(const std::string& host, std::uint16_t port, std::string& err);
  // Queries the bound port (useful when port 0 was requested).
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] SocketHandle accept_one(std::string& err);
  // Bounds how long accept_one waits before reporting err == "timeout".
  void set_timeout_ms(std::uint32_t ms) noexcept;
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }

 private:
  SocketHandle handle_{kInvalidSocket};
  std::uint16_t port_{0};
};

class TcpStream {
 public:
  TcpStream() = default;
  explicit TcpStream(SocketHandle h) noexcept : handle_(h) {}
  ~TcpStream();
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;
  TcpStream(TcpStream&& other) noexcept;
  TcpStream& operator=(TcpStream&& other) noexcept;

  [[nodiscard]] static bool connect_to(const std::string& host, std::uint16_t port, TcpStream& out,
                                       std::string& err);
  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }
  void set_timeout_ms(std::uint32_t ms) noexcept;
  void close() noexcept;

  [[nodiscard]] bool send_bytes(const std::uint8_t* data, std::size_t size, std::string& err);
  [[nodiscard]] bool send_frame(const Frame& frame, std::string& err);
  // Reads exactly one frame. On peer close returns false with err == "peer closed".
  [[nodiscard]] bool recv_frame(Frame& frame, std::string& err);
  // Non-blocking probe used to detect a dead peer without ever blocking.
  [[nodiscard]] bool peer_alive() noexcept;

 private:
  SocketHandle handle_{kInvalidSocket};
};

}  // namespace congestion_recovery
