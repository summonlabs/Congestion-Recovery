// Congestion Recovery - versioned durable snapshot and append-only journal.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/plan.hpp"
#include "congestion_recovery/policy.hpp"

namespace congestion_recovery {

inline constexpr std::uint32_t kSnapshotFormatVersion = 1;
inline constexpr std::uint32_t kJournalFormatVersion = 1;
inline constexpr std::uint64_t kSnapshotMaxBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kJournalMaxBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kJournalMaxRecordBytes = 4ull * 1024ull * 1024ull;

// What a load actually restored. Durable configuration and committed history
// survive; live authority, liveness, telemetry freshness and in-flight work do
// not, and are reported explicitly.
struct RestoreReport {
  bool loaded{false};
  bool valid{true};
  bool recovered_from_journal{false};
  std::string detail{};
  std::uint64_t journal_records_replayed{0};
  std::uint64_t journal_records_rejected{0};
  std::uint64_t journal_truncated_bytes{0};
  std::uint32_t policies_loaded{0};
  std::uint32_t plans_loaded{0};
  std::uint32_t plans_requiring_revalidation{0};
  std::uint32_t completions_preserved{0};
  std::uint32_t completions_failed_preserved{0};
  std::uint32_t rollbacks_preserved{0};
  std::uint32_t authorities_dropped{0};
  std::uint64_t epoch_before{0};
  std::uint64_t epoch_after{0};
  std::vector<RecoveryPlanId> revalidation_plans{};
};

// Durable container handed to the engine on restart.
struct DurableState {
  std::uint32_t format_version{kSnapshotFormatVersion};
  CoordinatorEpoch epoch{};
  std::uint64_t snapshot_tick{0};
  // Highest journal sequence already reflected in this snapshot. Replay starts
  // strictly after it.
  std::uint64_t journal_sequence{0};
  std::vector<RecoveryPolicy> policies{};
  std::vector<RecoveryPlan> plans{};
};

// Append-only, checksummed journal record. Sequence numbers never repeat and
// never regress.
struct JournalRecord {
  std::uint64_t sequence{0};
  std::uint64_t tick{0};
  CoordinatorEpoch epoch{};
  std::string kind{};
  std::vector<std::uint8_t> payload{};
};

// Computes the CRC-32 (IEEE) of a byte range.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

// Serialize durable state into a self-describing, integrity-checked buffer.
[[nodiscard]] bool serialize_state(const DurableState& state, const EngineLimits& limits,
                                   std::vector<std::uint8_t>& out, std::string& err);

// Parse and strictly validate a snapshot buffer. Rejects bad magic, unknown
// version, truncation, checksum mismatch, trailing bytes, out-of-range enums,
// generation regressions and impossible accounting.
[[nodiscard]] bool deserialize_state(const std::uint8_t* data, std::size_t size, const EngineLimits& limits,
                                     DurableState& out, std::string& err);

// Encode one journal record (without the stream frame header).
[[nodiscard]] bool serialize_journal_record(const JournalRecord& record, std::vector<std::uint8_t>& out,
                                            std::string& err);

// Replay a journal byte stream. Truncation at a record boundary is reported as
// journal_truncated_bytes (a crash mid-append is expected); corruption inside a
// complete record is a rejection. Records with a stale epoch are counted as
// rejected.
[[nodiscard]] bool replay_journal(const std::uint8_t* data, std::size_t size, CoordinatorEpoch live_epoch,
                                  const EngineLimits& limits, std::vector<JournalRecord>& out,
                                  RestoreReport& report, std::string& err);

}  // namespace congestion_recovery
