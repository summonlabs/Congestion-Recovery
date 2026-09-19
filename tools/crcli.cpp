// Congestion Recovery - crcli: inspect durable recovery state offline.
// Copyright 2026 Summon Software Labs.
//
// Inspection is read-only with respect to the durable store: the state is
// loaded into a fresh engine under a new epoch, and no file is written.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "congestion_recovery/engine.hpp"
#include "congestion_recovery/journal.hpp"

namespace {

void usage() {
  std::printf(
      "crcli --state DIR [--plan ID] [--quiet]\n"
      "Loads a durable recovery snapshot and prints what restart actually restores.\n");
}

bool take_value(int argc, char** argv, int& i, std::string& out) {
  if (i + 1 >= argc) {
    return false;
  }
  out = argv[++i];
  return true;
}

std::vector<std::uint8_t> read_file(const std::string& path, bool& ok) {
  std::vector<std::uint8_t> bytes;
  ok = false;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return bytes;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > congestion_recovery::kSnapshotMaxBytes) {
    return bytes;
  }
  in.seekg(0, std::ios::beg);
  bytes.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
      bytes.clear();
      return bytes;
    }
  }
  ok = true;
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace congestion_recovery;
  std::string state_dir{"."};
  std::uint64_t plan_filter = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string value;
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--state") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      state_dir = value;
    } else if (arg == "--plan") {
      if (!take_value(argc, argv, i, value)) { usage(); return 2; }
      plan_filter = std::strtoull(value.c_str(), nullptr, 10);
    } else {
      std::printf("unknown argument: %s\n", arg.c_str());
      usage();
      return 2;
    }
  }

  const std::string snapshot_path = state_dir + "/recovery.snapshot";
  bool ok = false;
  const std::vector<std::uint8_t> bytes = read_file(snapshot_path, ok);
  if (!ok) {
    std::printf("CLI_ERROR detail=snapshot could not be read: %s\n", snapshot_path.c_str());
    return 1;
  }

  const EngineLimits limits{};
  DurableState state{};
  std::string err;
  if (!deserialize_state(bytes.data(), bytes.size(), limits, state, err)) {
    std::printf("CLI_ERROR detail=snapshot rejected: %s\n", err.c_str());
    return 1;
  }
  std::printf("SNAPSHOT version=%u epoch=%llu tick=%llu policies=%llu plans=%llu\n", state.format_version,
              static_cast<unsigned long long>(state.epoch.value()),
              static_cast<unsigned long long>(state.snapshot_tick),
              static_cast<unsigned long long>(state.policies.size()),
              static_cast<unsigned long long>(state.plans.size()));

  RecoveryEngine engine{limits};
  const RestoreReport report = engine.import_state(state, CoordinatorEpoch(state.epoch.value() + 1u));
  if (!report.valid) {
    std::printf("CLI_ERROR detail=restore rejected: %s\n", report.detail.c_str());
    return 1;
  }
  std::printf(
      "RESTORE policies=%u plans=%u requiring_revalidation=%u completions_preserved=%u "
      "failed_completions_preserved=%u rollbacks_preserved=%u authorities_dropped=%u "
      "epoch_before=%llu epoch_after=%llu\n",
      report.policies_loaded, report.plans_loaded, report.plans_requiring_revalidation,
      report.completions_preserved, report.completions_failed_preserved, report.rollbacks_preserved,
      report.authorities_dropped, static_cast<unsigned long long>(report.epoch_before),
      static_cast<unsigned long long>(report.epoch_after));
  std::printf("RESTORE_NOTE %s\n", report.detail.c_str());

  for (const RecoveryPlanId id : engine.plan_ids()) {
    if (plan_filter != 0u && id.value() != plan_filter) {
      continue;
    }
    const RecoveryPlan* plan = engine.plan(id);
    if (plan == nullptr) {
      continue;
    }
    std::printf(
        "PLAN id=%llu generation=%llu epoch=%llu state=%s stage=%llu attempts=%u rollbacks=%u "
        "completions=%u revalidation_required=%s\n",
        static_cast<unsigned long long>(id.value()),
        static_cast<unsigned long long>(plan->generation.value()),
        static_cast<unsigned long long>(plan->epoch.value()), std::string(to_string(plan->state)).c_str(),
        static_cast<unsigned long long>(plan->stage_index), plan->total_attempts, plan->rollback_count,
        plan->completion_count, plan->revalidation_required ? "yes" : "no");
    std::printf("%s", engine.render_explanation(id).c_str());
  }
  std::printf("CLI_OK\n");
  return 0;
}
