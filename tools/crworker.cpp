// Congestion Recovery - crworker: evidence publisher and stage applier process.
// Copyright 2026 Summon Software Labs.
//
// The worker never decides that recovery advanced. It applies bounded stage
// grants, reports what it observed, and dies when told to.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "congestion_recovery/protocol.hpp"

namespace {

using namespace congestion_recovery;

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint64_t worker{1};
  std::uint64_t boot{1};
  std::string role{"applier"};
  int die_before_report{0};
  std::string record_path{};
  std::string replay_path{};
};

void usage() {
  std::printf(
      "crworker --port P --role applier|observer [--host H] [--worker N] [--boot N]\n"
      "         [--die-before-report K] [--record FILE]\n"
      "crworker --replay FILE [--host H] [--port P]\n");
}

bool take_value(int argc, char** argv, int& i, std::string& out) {
  if (i + 1 >= argc) {
    return false;
  }
  out = argv[++i];
  return true;
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2u);
  for (const std::uint8_t byte : bytes) {
    out.push_back(digits[(byte >> 4) & 0x0fu]);
    out.push_back(digits[byte & 0x0fu]);
  }
  return out;
}

bool from_hex(const std::string& text, std::vector<std::uint8_t>& out) {
  if (text.empty() || text.size() % 2u != 0u) {
    return false;
  }
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
  };
  out.clear();
  out.reserve(text.size() / 2u);
  for (std::size_t i = 0; i < text.size(); i += 2u) {
    const int hi = value(text[i]);
    const int lo = value(text[i + 1u]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

int run_replay(const Options& options) {
  std::ifstream in(options.replay_path);
  if (!in) {
    std::printf("REPLAY_ERROR detail=record file could not be opened\n");
    return 2;
  }
  std::vector<std::vector<std::uint8_t>> recorded;
  std::string text;
  while (std::getline(in, text)) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
      text.pop_back();
    }
    if (text.empty()) {
      continue;
    }
    std::vector<std::uint8_t> bytes;
    if (!from_hex(text, bytes)) {
      std::printf("REPLAY_ERROR detail=record file is malformed\n");
      return 2;
    }
    recorded.push_back(std::move(bytes));
  }
  if (recorded.empty()) {
    std::printf("REPLAY_ERROR detail=record file is empty\n");
    return 2;
  }

  Frame first{};
  std::size_t consumed = 0;
  std::string err;
  if (!decode_frame(recorded.front().data(), recorded.front().size(), first, consumed, err)) {
    std::printf("REPLAY_ERROR detail=recorded frame is malformed\n");
    return 2;
  }

  std::string startup_err;
  if (!socket_system_startup(startup_err)) {
    std::printf("REPLAY_ERROR detail=socket startup failed\n");
    return 2;
  }
  TcpStream stream{};
  if (!TcpStream::connect_to(options.host, options.port, stream, err)) {
    std::printf("REPLAY_ERROR detail=connect failed\n");
    socket_system_shutdown();
    return 2;
  }
  stream.set_timeout_ms(8000);

  Frame hello{};
  hello.kind = MessageKind::HELLO;
  hello.epoch = first.epoch;
  hello.worker = first.worker;
  hello.boot = first.boot;
  Writer body;
  body.put_u32(kProtocolVersion);
  body.put_str("replay");
  body.put_str("127.0.0.1");
  hello.body = body.buffer();
  if (!stream.send_frame(hello, err)) {
    std::printf("REPLAY_ERROR detail=hello send failed\n");
    socket_system_shutdown();
    return 2;
  }
  Frame hello_reply{};
  if (!stream.recv_frame(hello_reply, err)) {
    std::printf("REPLAY_ERROR detail=hello reply failed\n");
    socket_system_shutdown();
    return 2;
  }
  if (hello_reply.kind == MessageKind::ERROR_MSG) {
    Reader reader(hello_reply.body.data(), hello_reply.body.size());
    const WireError wire = static_cast<WireError>(reader.get_u8());
    std::printf("REPLAY_HELLO_REJECT worker=%llu boot=%llu reason=%s\n",
                static_cast<unsigned long long>(first.worker.value()),
                static_cast<unsigned long long>(first.boot.value()),
                std::string(to_string(wire)).c_str());
  } else {
    std::printf("REPLAY_HELLO_ACCEPTED worker=%llu boot=%llu\n",
                static_cast<unsigned long long>(first.worker.value()),
                static_cast<unsigned long long>(first.boot.value()));
  }

  for (std::size_t i = 0; i < recorded.size(); ++i) {
    Frame frame{};
    std::size_t used = 0;
    if (!decode_frame(recorded[i].data(), recorded[i].size(), frame, used, err)) {
      break;
    }
    if (frame.kind == MessageKind::HELLO) {
      continue;
    }
    if (!stream.send_bytes(recorded[i].data(), recorded[i].size(), err)) {
      std::printf("REPLAY_FRAME kind=%s result=SEND_FAILED\n", std::string(to_string(frame.kind)).c_str());
      break;
    }
    Frame reply{};
    if (!stream.recv_frame(reply, err)) {
      std::printf("REPLAY_FRAME kind=%s result=NO_REPLY\n", std::string(to_string(frame.kind)).c_str());
      break;
    }
    if (reply.kind == MessageKind::ERROR_MSG) {
      Reader reader(reply.body.data(), reply.body.size());
      const WireError wire = static_cast<WireError>(reader.get_u8());
      std::printf("REPLAY_FRAME kind=%s epoch=%llu result=REJECT reason=%s\n",
                  std::string(to_string(frame.kind)).c_str(),
                  static_cast<unsigned long long>(frame.epoch.value()),
                  std::string(to_string(wire)).c_str());
    } else {
      std::printf("REPLAY_FRAME kind=%s result=ACCEPTED reply=%s\n",
                  std::string(to_string(frame.kind)).c_str(),
                  std::string(to_string(reply.kind)).c_str());
    }
    break;  // one replayed frame is enough to prove the rejection
  }

  stream.close();
  socket_system_shutdown();
  std::printf("REPLAY_DONE\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--replay") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.replay_path = value;
    } else if (arg == "--host") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.host = value;
    } else if (arg == "--port") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (arg == "--worker") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.worker = std::strtoull(value.c_str(), nullptr, 10);
    } else if (arg == "--boot") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.boot = std::strtoull(value.c_str(), nullptr, 10);
    } else if (arg == "--role") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.role = value;
    } else if (arg == "--die-before-report") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.die_before_report = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
    } else if (arg == "--record") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      options.record_path = value;
    } else {
      std::printf("unknown argument: %s\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (!options.replay_path.empty()) {
    return run_replay(options);
  }
  if (options.port == 0u) {
    usage();
    return 2;
  }
  if (options.role != "applier" && options.role != "observer") {
    std::printf("role must be applier or observer\n");
    return 2;
  }

  std::string err;
  if (!socket_system_startup(err)) {
    std::printf("WORKER_ERROR detail=socket startup failed\n");
    return 2;
  }
  TcpStream stream{};
  if (!TcpStream::connect_to(options.host, options.port, stream, err)) {
    std::printf("WORKER_ERROR detail=connect failed\n");
    socket_system_shutdown();
    return 2;
  }
  stream.set_timeout_ms(8000);

  Frame hello{};
  hello.kind = MessageKind::HELLO;
  hello.epoch = CoordinatorEpoch(1);
  hello.worker = WorkerId(options.worker);
  hello.boot = WorkerBoot(options.boot);
  Writer hello_body;
  hello_body.put_u32(kProtocolVersion);
  hello_body.put_str(options.role);
  hello_body.put_str(options.host);
  hello.body = hello_body.buffer();
  if (!stream.send_frame(hello, err)) {
    std::printf("WORKER_ERROR detail=hello send failed\n");
    socket_system_shutdown();
    return 2;
  }
  Frame ack{};
  if (!stream.recv_frame(ack, err)) {
    std::printf("WORKER_ERROR detail=hello reply failed\n");
    socket_system_shutdown();
    return 2;
  }
  if (ack.kind == MessageKind::ERROR_MSG) {
    Reader reader(ack.body.data(), ack.body.size());
    const WireError wire = static_cast<WireError>(reader.get_u8());
    std::printf("HELLO_REFUSED worker=%llu boot=%llu reason=%s\n",
                static_cast<unsigned long long>(options.worker),
                static_cast<unsigned long long>(options.boot),
                std::string(to_string(wire)).c_str());
    stream.close();
    socket_system_shutdown();
    return 0;
  }
  std::printf("WORKER worker=%llu boot=%llu role=%s registered=1 epoch=%llu\n",
              static_cast<unsigned long long>(options.worker),
              static_cast<unsigned long long>(options.boot), options.role.c_str(),
              static_cast<unsigned long long>(ack.epoch.value()));

  std::ofstream record;
  if (!options.record_path.empty()) {
    record.open(options.record_path, std::ios::trunc);
  }

  int grant_ordinal = 0;
  for (;;) {
    Frame frame{};
    if (!stream.recv_frame(frame, err)) {
      std::printf("WORKER_EXIT detail=%s\n", err.c_str());
      stream.close();
      socket_system_shutdown();
      return 0;
    }
    if (frame.kind == MessageKind::GOODBYE) {
      std::printf("GOODBYE worker=%llu\n", static_cast<unsigned long long>(options.worker));
      break;
    }
    if (frame.kind == MessageKind::FENCE || frame.kind == MessageKind::ERROR_MSG) {
      Reader reader(frame.body.data(), frame.body.size());
      const WireError wire = static_cast<WireError>(reader.get_u8());
      std::printf("FENCED worker=%llu reason=%s\n", static_cast<unsigned long long>(options.worker),
                  std::string(to_string(wire)).c_str());
      break;
    }
    if (frame.kind == MessageKind::EVIDENCE_REQUEST) {
      Reader reader(frame.body.data(), frame.body.size());
      const std::uint64_t tick = reader.get_u64();
      const std::uint8_t mode = reader.get_u8();
      const std::uint32_t kinds = reader.get_u32();
      std::vector<std::uint8_t> requested;
      for (std::uint32_t i = 0; i < kinds && !reader.bad(); ++i) {
        requested.push_back(reader.get_u8());
      }
      if (reader.bad()) {
        std::printf("WORKER_ERROR detail=malformed evidence request\n");
        break;
      }
      Frame reply{};
      reply.kind = MessageKind::EVIDENCE_PUBLISH;
      reply.epoch = frame.epoch;
      reply.worker = frame.worker;
      reply.boot = frame.boot;
      reply.sequence = frame.sequence;
      Writer body;
      body.put_u64(tick);
      body.put_u32(static_cast<std::uint32_t>(requested.size()));
      for (const std::uint8_t kind : requested) {
        const EvidenceKind evidence_kind = static_cast<EvidenceKind>(kind);
        std::uint64_t value = 0;
        bool affirmative = false;
        if (evidence_kind == EvidenceKind::CONGESTION_CLEARED) {
          const bool cleared = mode == 0u;
          value = cleared ? 1u : 0u;
          affirmative = cleared;
        } else if (evidence_kind == EvidenceKind::SERVICE_RATE) {
          value = mode == 0u ? 2000u : 100u;
          affirmative = true;
        }
        body.put_u8(kind);
        body.put_u64(0u);
        body.put_u64(value);
        body.put_u64(4u);
        body.put_u64(30u);
        body.put_f64(0.9);
        body.put_u8(static_cast<std::uint8_t>(Provenance::MEASURED));
        body.put_u8(1u);
        body.put_u8(affirmative ? 1u : 0u);
      }
      reply.body = body.buffer();
      if (!stream.send_frame(reply, err)) {
        std::printf("WORKER_ERROR detail=evidence send failed\n");
        break;
      }
      std::printf("EVIDENCE tick=%llu mode=%u count=%u\n", static_cast<unsigned long long>(tick),
                  static_cast<unsigned>(mode), static_cast<unsigned>(requested.size()));
      continue;
    }
    if (frame.kind == MessageKind::STAGE_GRANT) {
      ++grant_ordinal;
      if (options.die_before_report > 0 && grant_ordinal == options.die_before_report) {
        std::printf("DIED worker=%llu boot=%llu grant=%d\n",
                    static_cast<unsigned long long>(options.worker),
                    static_cast<unsigned long long>(options.boot), grant_ordinal);
        std::fflush(stdout);
        stream.close();
        socket_system_shutdown();
        std::_Exit(9);  // hard death before any report: the attempt is ambiguous
      }
      Reader reader(frame.body.data(), frame.body.size());
      const std::uint64_t plan = reader.get_u64();
      const std::uint64_t plan_generation = reader.get_u64();
      const std::uint64_t stage_generation = reader.get_u64();
      const std::uint64_t attempt = reader.get_u64();
      const std::uint8_t stage_kind = reader.get_u8();
      const std::uint64_t tick = reader.get_u64();
      const std::uint32_t count = reader.get_u32();
      struct Level {
        std::uint64_t resource;
        std::uint8_t unit;
        std::uint64_t effective;
      };
      std::vector<Level> levels;
      for (std::uint32_t i = 0; i < count && !reader.bad(); ++i) {
        const std::uint64_t resource = reader.get_u64();
        const std::uint8_t unit = reader.get_u8();
        const std::uint64_t requested = reader.get_u64();
        (void)reader.get_u64();
        levels.push_back(Level{resource, unit, requested});
      }
      const std::uint64_t authority_generation = reader.get_u64();
      if (reader.bad()) {
        std::printf("WORKER_ERROR detail=malformed stage grant\n");
        break;
      }
      (void)stage_generation;
      (void)stage_kind;

      Frame reply{};
      reply.kind = MessageKind::STAGE_REPORT;
      reply.epoch = frame.epoch;
      reply.worker = frame.worker;
      reply.boot = frame.boot;
      reply.sequence = frame.sequence;
      Writer body;
      body.put_u64(plan);
      body.put_u64(plan_generation);
      body.put_u64(attempt);
      body.put_u8(0u);
      body.put_u32(static_cast<std::uint32_t>(levels.size()));
      for (const Level& level : levels) {
        body.put_u64(level.resource);
        body.put_u8(level.unit);
        body.put_u64(level.effective);
        body.put_u64(level.effective);
      }
      reply.body = body.buffer();

      std::vector<std::uint8_t> encoded;
      std::string encode_err;
      if (!encode_frame(reply, encoded, encode_err)) {
        std::printf("WORKER_ERROR detail=%s\n", encode_err.c_str());
        break;
      }
      if (record.is_open()) {
        record << to_hex(encoded) << "\n";
        record.flush();
      }
      if (!stream.send_bytes(encoded.data(), encoded.size(), err)) {
        std::printf("WORKER_ERROR detail=report send failed\n");
        break;
      }
      std::printf("REPORTED tick=%llu attempt=%llu levels=%u authority=%llu\n",
                  static_cast<unsigned long long>(tick),
                  static_cast<unsigned long long>(attempt), static_cast<unsigned>(levels.size()),
                  static_cast<unsigned long long>(authority_generation));
      continue;
    }
    std::printf("WORKER_IGNORED kind=%s\n", std::string(to_string(frame.kind)).c_str());
  }

  stream.close();
  socket_system_shutdown();
  return 0;
}
