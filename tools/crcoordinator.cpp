// Congestion Recovery - crcoordinator: recovery authority holder process.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "congestion_recovery/coordinator.hpp"

namespace {

void usage() {
  std::printf(
      "crcoordinator --state DIR [--host H] [--port P] [--scenario NAME] [--workers N]\n"
      "              [--report FILE] [--peer-timeout-ms N] [--tick-step N]\n"
      "scenarios: staged | recurrence | worker-death | restart-part1 | restart-part2\n"
      "port 0 selects an ephemeral port and prints the chosen one.\n");
}

bool take_value(int argc, char** argv, int& i, std::string& out) {
  if (i + 1 >= argc) {
    return false;
  }
  out = argv[++i];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  congestion_recovery::CoordinatorConfig config{};
  config.state_dir = ".";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--host") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.host = value;
    } else if (arg == "--port") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (arg == "--state") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.state_dir = value;
    } else if (arg == "--scenario") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.scenario = value;
    } else if (arg == "--workers") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.expected_workers = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (arg == "--peer-timeout-ms") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.peer_timeout_ms = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (arg == "--tick-step") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.tick_step = std::strtoull(value.c_str(), nullptr, 10);
    } else if (arg == "--report") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      config.report_path = value;
    } else {
      std::printf("unknown argument: %s\n", arg.c_str());
      usage();
      return 2;
    }
  }
  if (config.expected_workers == 0u || config.expected_workers > 8u) {
    std::printf("workers must be between 1 and 8\n");
    return 2;
  }
  congestion_recovery::Coordinator coordinator(config);
  return coordinator.run();
}
