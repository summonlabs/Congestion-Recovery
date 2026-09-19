// Congestion Recovery - generation-bound recovery evidence.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "congestion_recovery/checked.hpp"
#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

// A single observation supplied by a publisher. It is bound to the exact plan,
// stage, epoch and resource generations it was collected under, so it can be
// rejected as stale instead of being silently used.
struct Evidence {
  EvidenceId id{};
  EvidenceGeneration generation{};
  EvidenceKind kind{EvidenceKind::UNKNOWN};
  ResourceId resource{};
  PlanGeneration plan_generation{};
  StageGeneration stage_generation{};
  CoordinatorEpoch epoch{};
  ResourceGeneration resource_generation{};
  PolicyGeneration policy_generation{};
  PublisherId publisher{};
  WorkerId worker{};
  WorkerBoot boot{};
  PublisherIncarnation incarnation{};
  Provenance provenance{Provenance::UNKNOWN};
  double confidence{0.0};
  std::uint64_t observed_at_tick{0};
  std::uint64_t duration_ticks{0};
  std::uint64_t sample_count{0};
  std::uint64_t value{0};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  bool stable{false};
  bool affirmative{false};
};

// A bounded, generation-stamped set of evidence evaluated as one unit.
struct EvidenceSnapshot {
  EvidenceSnapshotId id{};
  SnapshotGeneration generation{};
  PlanGeneration plan_generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch epoch{};
  std::uint64_t assembled_at_tick{0};
  std::vector<Evidence> entries{};

  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

// What a stage demands before it may be left.
struct EvidenceRequirement {
  EvidenceKind kind{EvidenceKind::UNKNOWN};
  Provenance min_provenance{Provenance::UNKNOWN};
  double min_confidence{0.0};
  std::uint64_t min_samples{1};
  std::uint64_t min_duration_ticks{0};
  std::uint64_t stability_window_ticks{0};
  std::uint64_t min_value{0};
  std::uint64_t max_value{0};
  bool has_min_value{false};
  bool has_max_value{false};
  bool require_stable{false};
  bool require_affirmative{false};
  bool require_bound_resource{false};
  // Exactly one requirement per stage is the primary: its value drives the
  // hysteresis band that decides advance / hold / recurrence.
  bool primary{false};
};

// Deterministic outcome of evaluating one requirement.
struct EvidenceEvaluation {
  EvidenceKind kind{EvidenceKind::UNKNOWN};
  ResourceId resource{};
  bool satisfied{false};
  bool any_fresh{false};
  bool contradictory{false};
  std::uint64_t samples{0};
  std::uint64_t duration_ticks{0};
  std::uint64_t observed_value{0};
  double confidence{0.0};
  Provenance best_provenance{Provenance::UNKNOWN};
  bool stable{false};
  bool latest_affirmative{false};
  std::string detail{};
};

}  // namespace congestion_recovery
