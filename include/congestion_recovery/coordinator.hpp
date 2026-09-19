// Congestion Recovery - recovery coordinator (recovery authority holder).
// Copyright 2026 Summon Software Labs.
//
// The coordinator owns the recovery epoch, the plan store and the durable
// journal. Workers publish evidence and apply bounded stage grants; they never
// decide that recovery advanced. The coordinator is single-threaded and never
// holds a mutex across socket I/O or durable writes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/engine.hpp"
#include "congestion_recovery/journal.hpp"
#include "congestion_recovery/policy.hpp"
#include "congestion_recovery/protocol.hpp"

namespace congestion_recovery {

struct CoordinatorConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string state_dir{};
  std::string report_path{};
  std::string scenario{"staged"};
  std::uint32_t expected_workers{2};
  std::uint32_t peer_timeout_ms{8000};
  std::uint64_t tick_step{10};
  bool verbose{false};
};

// Result of one coordinator run, rendered as deterministic key/value lines so
// external tests assert on exact facts rather than prose.
struct CoordinatorReport {
  std::string scenario{};
  bool success{false};
  std::uint64_t epoch_before{0};
  std::uint64_t epoch_after{0};
  std::uint32_t workers_registered{0};
  std::uint32_t workers_fenced{0};
  std::uint32_t stages_completed{0};
  std::uint32_t rollbacks{0};
  std::uint32_t completions_committed{0};
  std::uint32_t stale_rejections{0};
  std::uint32_t revalidations{0};
  std::uint32_t restore_plans_requiring_revalidation{0};
  std::uint64_t journal_records{0};
  std::string plan_state{};
  std::vector<std::string> lines{};

  [[nodiscard]] std::string render() const;
  [[nodiscard]] bool write(const std::string& path, std::string& err) const;
};

// Durable journal + snapshot store with an explicit write-ahead protocol:
//   append intent -> flush -> apply in memory -> write snapshot atomically
// A crash between intent and commit leaves an unfinished attempt that restart
// reports as ambiguous instead of silently successful.
class DurableStore {
 public:
  DurableStore(std::string dir, EngineLimits limits);
  ~DurableStore();

  [[nodiscard]] bool open(std::string& err);
  [[nodiscard]] bool append(const JournalRecord& record, std::string& err);
  [[nodiscard]] bool flush(std::string& err);
  [[nodiscard]] bool write_snapshot(const DurableState& state, std::string& err);
  [[nodiscard]] bool load(DurableState& state, RestoreReport& report, std::string& err);
  [[nodiscard]] const std::string& dir() const noexcept { return dir_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }
  [[nodiscard]] std::uint64_t open_attempt_epoch() const noexcept { return open_attempt_epoch_; }

 private:
  std::string dir_{};
  std::string snapshot_path_{};
  std::string journal_path_{};
  EngineLimits limits_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t records_written_{0};
  std::uint64_t open_attempt_epoch_{0};
};

// Runs one scripted recovery scenario end to end against real worker processes.
class Coordinator {
 public:
  explicit Coordinator(CoordinatorConfig config);
  ~Coordinator();

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  [[nodiscard]] int run();
  [[nodiscard]] const CoordinatorReport& report() const noexcept { return report_; }

 private:
  struct Peer;

  [[nodiscard]] bool setup(std::string& err);
  [[nodiscard]] bool accept_workers(std::string& err);
  [[nodiscard]] bool accept_extra(std::string& err);
  [[nodiscard]] bool register_connection(TcpStream&& stream, std::string& err);
  [[nodiscard]] bool accept_replacement(const std::string& role, std::string& err);
  [[nodiscard]] Peer* find_peer(const std::string& role);
  [[nodiscard]] bool run_staged(std::string& err, bool recurrence_mode, std::uint32_t stop_after);
  [[nodiscard]] bool grant_stage(Peer& peer, std::uint64_t tick, std::string& err);
  [[nodiscard]] bool collect_evidence(Peer& peer, std::uint64_t tick, bool recurrence,
                                      const RestorationVector& applied, std::string& err);
  [[nodiscard]] bool publish_and_advance(Peer& peer, std::uint64_t tick, bool recurrence,
                                         const RestorationVector& applied, std::string& err);
  [[nodiscard]] bool commit(const std::string& kind, std::uint64_t tick, std::string& err);
  [[nodiscard]] bool grant_authority_for_plan(RecoveryPlanId id);
  [[nodiscard]] int finish();
  void line(const std::string& text);
  void fail(const std::string& text);

  CoordinatorConfig config_{};
  CoordinatorReport report_{};
  EngineLimits limits_{};
  RecoveryEngine engine_{};
  DurableStore store_;
  std::vector<Peer> peers_{};
  TcpListener listener_{};
  std::uint64_t tick_{0};
  std::uint64_t plan_id_{0};
  std::uint64_t epoch_before_{0};
  bool success_{true};
};

}  // namespace congestion_recovery
