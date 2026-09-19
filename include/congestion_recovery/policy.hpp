// Congestion Recovery - recovery policy and stage ladder.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/evidence.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

// Hard bounds for externally supplied policy values.
struct EngineLimits {
  std::uint32_t max_policies{64};
  std::uint32_t max_plans{4096};
  std::uint32_t max_stages_per_policy{16};
  std::uint32_t max_resources_per_plan{256};
  std::uint32_t max_obligations_per_plan{64};
  std::uint32_t max_evidence_entries{512};
  std::uint32_t max_requirements_per_stage{16};
  std::uint32_t max_stage_history{64};
  std::uint32_t max_rollback_history{32};
  std::uint32_t max_completion_history{64};
  std::uint32_t max_explanation_items{64};
  std::uint32_t max_reason_length{256};
  std::uint32_t max_adjacent_requests{128};
  std::uint32_t max_attempts_per_stage{16};
  std::uint32_t max_total_attempts{64};
  std::uint32_t max_rollbacks{8};
  std::uint64_t max_ticks{1000000000ull};

  [[nodiscard]] bool valid() const noexcept;
};

// One rung of the restoration ladder.
struct StageSpec {
  StageKind kind{StageKind::UNKNOWN};
  std::string name{};
  RestorationVector target{};             // absolute requested levels for this stage
  std::uint64_t dwell_ticks{0};           // minimum time in stage before advancing
  std::uint64_t stability_window_ticks{0};
  std::uint32_t max_attempts{3};
  bool requires_authority{true};
  bool rollback_on_failure{true};
  std::vector<EvidenceRequirement> requirements{};
};

// Bounded hysteresis. The advance band and the rollback band are separated by
// the hold band, so evidence inside the hold band can never oscillate.
struct HysteresisPolicy {
  std::uint64_t advance_margin{0};
  std::uint64_t rollback_margin{0};
  std::uint64_t cooldown_ticks{0};
  std::uint32_t max_transitions_per_window{4};
  std::uint64_t window_ticks{1000};
};

struct RecoveryPolicy {
  PolicyId id{};
  PolicyGeneration generation{};
  std::string name{};
  std::vector<StageSpec> stages{};
  HysteresisPolicy hysteresis{};
  RecurrenceAction on_recurrence{RecurrenceAction::ROLLBACK};
  std::uint64_t max_evidence_age_ticks{100};
  std::uint64_t max_plan_lifetime_ticks{1000000};
  std::uint64_t revalidation_boundary_ticks{10000};
  std::uint32_t max_rollbacks{3};
  std::uint32_t max_total_attempts{16};
  // Completion tolerance in basis points: 0 requires the full effective level.
  std::uint32_t partial_application_tolerance_bp{0};
  bool baseline_required{false};
  bool require_completion_evidence{true};
  bool require_stable_completion{true};
};

// Structural + bound validation. On failure err receives a bounded reason.
[[nodiscard]] bool validate_policy(const RecoveryPolicy& policy, const EngineLimits& limits, std::string& err);

// The stage that answers "is it safe to restore now".
[[nodiscard]] const StageSpec* stage_at(const RecoveryPolicy& policy, std::size_t index) noexcept;

}  // namespace congestion_recovery
