// Congestion Recovery - recovery plan model, stage/rollback/completion lineage.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/authority.hpp"
#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/evidence.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/obligations.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

// A resource is only usable at an exact generation. A path change or capacity
// reconfiguration bumps it and invalidates every plan bound to the old value.
struct ResourceBinding {
  ResourceId resource{};
  ResourceGeneration generation{};
};

// Lineage record for one stage occupancy. Binds the exact evidence and
// authority generations that justified leaving (or failing) the stage.
struct StageRecord {
  std::size_t index{0};
  StageKind kind{StageKind::UNKNOWN};
  StageGeneration generation{};
  AttemptId attempt{};
  std::uint64_t entered_tick{0};
  std::uint64_t exited_tick{0};
  StageOutcome outcome{StageOutcome::PENDING};
  std::uint32_t attempts{0};
  EvidenceGeneration evidence_generation{};
  AuthorityGeneration authority_generation{};
  CoordinatorEpoch epoch{};
  RestorationVector requested{};
  RestorationVector authorized{};
  RestorationVector observed{};
  RejectReason last_reason{RejectReason::NONE};
};

// Rollback history. The compensating plan is recomputed from the observed
// current state; steps whose observed level is unknown are listed, never
// guessed.
struct RollbackRecord {
  RollbackId id{};
  RollbackGeneration generation{};
  CoordinatorEpoch epoch{};
  RollbackKind kind{RollbackKind::UNKNOWN};
  RejectReason trigger{RejectReason::NONE};
  std::uint64_t tick{0};
  RestorationVector observed_current{};
  RestorationVector target{};
  RestorationVector compensation{};
  std::vector<ResourceId> unobserved{};
  std::uint32_t assumed_steps{0};
  std::uint32_t compensation_steps{0};
  std::uint32_t observed_steps{0};
  bool complete{false};
  std::string reason{};
};

// Completion lineage. A committed completion is durable and idempotent; a
// failed completion can never later be presented as success.
struct CompletionRecord {
  CompletionId id{};
  CompletionGeneration generation{};
  CoordinatorEpoch epoch{};
  std::size_t stage_index{0};
  StageGeneration stage_generation{};
  AttemptId attempt{};
  EvidenceGeneration evidence_generation{};
  CompletionState state{CompletionState::PENDING};
  std::uint64_t recorded_tick{0};
  std::uint64_t idempotency_key{0};
  RestorationVector applied{};
  RejectReason reject{RejectReason::NONE};
  bool replayed{false};
};

struct PlanHandle {
  RecoveryPlanId id{};
  PlanGeneration generation{};
  StageGeneration stage_generation{};

  [[nodiscard]] bool is_valid() const noexcept { return id.is_valid() && generation.is_valid(); }
};

struct CreatePlanRequest {
  PolicyId policy_id{};
  PolicyGeneration policy_generation{};
  CongestionRefId congestion{};
  InterventionId intervention{};
  CoordinatorEpoch epoch{};
  std::vector<ResourceBinding> resources{};
  RestorationVector baseline{};
  bool baseline_known{false};
  RestorationVector constrained{};
  RestorationVector rollback_target{};
  std::vector<ProtectedObligation> obligations{};
  std::uint64_t now_tick{0};
};

struct RecoveryPlan {
  RecoveryPlanId id{};
  PlanGeneration generation{};
  CoordinatorEpoch epoch{};
  PolicyId policy_id{};
  PolicyGeneration policy_generation{};
  CongestionRefId congestion{};
  InterventionId intervention{};
  std::vector<ResourceBinding> resources{};
  RestorationVector baseline{};
  bool baseline_known{false};
  RestorationVector constrained{};
  RestorationVector rollback_target{};
  RestorationVector authorized{};
  std::vector<ProtectedObligation> obligations{};
  RecoveryState state{RecoveryState::PLANNED};
  std::size_t stage_index{0};
  StageGeneration stage_generation{};
  AttemptId attempt{};
  std::uint64_t created_at_tick{0};
  std::uint64_t updated_at_tick{0};
  std::uint64_t last_transition_tick{0};
  std::uint64_t cooldown_until_tick{0};
  std::uint32_t attempts_in_stage{0};
  std::uint32_t total_attempts{0};
  std::uint32_t rollback_count{0};
  std::uint32_t completion_count{0};
  RejectReason last_reason{RejectReason::NONE};
  std::string last_reason_text{};
  bool revalidation_required{false};
  std::vector<std::string> revalidation_notes{};
  std::vector<StageRecord> stage_history{};
  std::vector<RollbackRecord> rollback_history{};
  std::vector<CompletionRecord> completions{};
  std::vector<std::uint64_t> transition_ticks{};
  RestorationVector last_observed{};

  [[nodiscard]] const ResourceBinding* binding(ResourceId resource) const noexcept {
    for (const ResourceBinding& b : resources) {
      if (b.resource == resource) {
        return &b;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool has_resource(ResourceId resource) const noexcept { return binding(resource) != nullptr; }
};

// Deterministic idempotency key over the exact facts that define a completion.
[[nodiscard]] std::uint64_t completion_key(RecoveryPlanId plan, PlanGeneration plan_generation,
                                           StageGeneration stage_generation, std::uint64_t evidence_generation,
                                           AttemptId attempt) noexcept;

// A snapshot's own generation is the evidence generation for any decision made
// from it, so decisions, records and idempotency keys bind the same value.
[[nodiscard]] constexpr EvidenceGeneration snapshot_evidence_generation(
    const EvidenceSnapshot& snapshot) noexcept {
  return EvidenceGeneration(snapshot.generation.value());
}

}  // namespace congestion_recovery
