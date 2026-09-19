// Congestion Recovery - deterministic recovery explanations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/authority.hpp"
#include "congestion_recovery/evidence.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/obligations.hpp"
#include "congestion_recovery/plan.hpp"
#include "congestion_recovery/policy.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

struct RestorationView {
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::uint64_t baseline{0};
  bool baseline_known{false};
  std::uint64_t constrained{0};
  std::uint64_t requested{0};
  std::uint64_t authorized{0};
  std::uint64_t effective{0};
  std::uint64_t observed{0};
  bool observed_known{false};
  std::uint64_t rollback_target{0};
  bool authority_clamped{false};
};

// Everything a caller needs to answer: what stage are we in, what proves it
// succeeded, how much was restored, what stayed protected, where would we roll
// back to, why are we stale or paused, and what authority do we hold.
struct RecoveryExplanation {
  RecoveryPlanId plan{};
  PlanGeneration plan_generation{};
  CoordinatorEpoch epoch{};
  PolicyId policy_id{};
  PolicyGeneration policy_generation{};
  RecoveryState state{RecoveryState::UNKNOWN};
  StageKind stage_kind{StageKind::UNKNOWN};
  std::size_t stage_index{0};
  std::size_t stage_count{0};
  std::string stage_name{};
  StageGeneration stage_generation{};
  AttemptId attempt{};
  std::vector<EvidenceEvaluation> required_evidence{};
  std::vector<EvidenceEvaluation> observed_evidence{};
  std::vector<RestorationView> restoration{};
  std::vector<ObligationEvaluation> obligations{};
  std::vector<AuthorityView> authority{};
  RejectReason stale_reason{RejectReason::NONE};
  RejectReason pause_reason{RejectReason::NONE};
  std::string reason_text{};
  std::uint64_t dwell_remaining_ticks{0};
  std::uint64_t cooldown_remaining_ticks{0};
  std::uint32_t attempts_in_stage{0};
  std::uint32_t total_attempts{0};
  std::uint32_t rollback_count{0};
  std::uint32_t completion_count{0};
  bool revalidation_required{false};
  std::vector<std::string> notes{};

  // Deterministic, line-oriented rendering used by tools and tests.
  [[nodiscard]] std::string render() const;
};

}  // namespace congestion_recovery
