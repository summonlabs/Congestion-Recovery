// Congestion Recovery - deterministic recovery engine.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "congestion_recovery/adjacent.hpp"
#include "congestion_recovery/authority.hpp"
#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/evidence.hpp"
#include "congestion_recovery/explain.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/journal.hpp"
#include "congestion_recovery/plan.hpp"
#include "congestion_recovery/policy.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

// Result of a deterministic stage evaluation. A decision is either an advance
// (or completion) or an explicit refusal carrying exactly one reason.
struct StageDecision {
  RecoveryPlanId plan{};
  PlanGeneration plan_generation{};
  CoordinatorEpoch epoch{};
  RecoveryDecision decision{RecoveryDecision::NONE};
  RejectReason reason{RejectReason::NONE};
  std::string reason_text{};
  RecoveryState state_before{RecoveryState::UNKNOWN};
  RecoveryState state_after{RecoveryState::UNKNOWN};
  std::size_t stage_index_before{0};
  std::size_t stage_index_after{0};
  StageKind stage_kind_before{StageKind::UNKNOWN};
  StageKind stage_kind_after{StageKind::UNKNOWN};
  StageGeneration stage_generation{};
  AttemptId attempt{};
  EvidenceGeneration evidence_generation{};
  AuthorityGeneration authority_generation{};
  std::uint64_t tick{0};
  bool advanced{false};
  bool completed{false};
  bool duplicate{false};
  bool clamped_by_authority{false};
  CompletionId completion{};
  CompletionGeneration completion_generation{};
  std::uint64_t idempotency_key{0};
  std::vector<EvidenceEvaluation> evidence{};
  std::vector<ObligationEvaluation> obligations{};
  std::vector<AuthorityView> authority{};
  RestorationVector effective{};
  RestorationVector observed{};

  // True when the transition was accepted without any refusal reason.
  [[nodiscard]] bool ok() const noexcept { return reason == RejectReason::NONE; }
};

struct RollbackRequest {
  RecoveryPlanId plan{};
  PlanGeneration plan_generation{};
  CoordinatorEpoch epoch{};
  RestorationVector observed_current{};
  Provenance provenance{Provenance::UNKNOWN};
  EvidenceGeneration evidence_generation{};
  std::uint64_t now_tick{0};
  RejectReason trigger{RejectReason::RECURRENCE_DETECTED};
  std::string reason{};
};

struct RollbackOutcome {
  bool ok{false};
  RejectReason reason{RejectReason::NONE};
  std::string reason_text{};
  RollbackId rollback{};
  RollbackGeneration generation{};
  CompensatingPlan compensation{};
  RecoveryState state_after{RecoveryState::UNKNOWN};
  bool plan_failed{false};
};

struct CompletionOutcome {
  bool ok{false};
  bool duplicate{false};
  CompletionState state{CompletionState::UNKNOWN};
  RejectReason reason{RejectReason::NONE};
  std::string reason_text{};
  CompletionId completion{};
  CompletionGeneration generation{};
  RecoveryState plan_state{RecoveryState::UNKNOWN};
  std::uint64_t idempotency_key{0};
};

struct RevalidationOutcome {
  bool ok{false};
  RejectReason reason{RejectReason::NONE};
  std::string reason_text{};
  RecoveryState plan_state{RecoveryState::UNKNOWN};
};

// Bounded durable-lineage event emitted by a committed mutation.
struct LineageEvent {
  std::string kind{};
  RecoveryPlanId plan{};
  PlanGeneration plan_generation{};
  CoordinatorEpoch epoch{};
  std::uint64_t tick{0};
  std::string detail{};
};

// The engine is deterministic: identical inputs produce identical decisions and
// identical explanations. It performs no I/O, owns no threads and never invokes
// a caller callback while holding its internal lock, so no re-entrancy path
// exists through this class.
class RecoveryEngine {
 public:
  explicit RecoveryEngine(EngineLimits limits = EngineLimits{});
  ~RecoveryEngine();

  RecoveryEngine(const RecoveryEngine&) = delete;
  RecoveryEngine& operator=(const RecoveryEngine&) = delete;

  [[nodiscard]] const EngineLimits& limits() const noexcept { return limits_; }

  // ---- policy ------------------------------------------------------------
  [[nodiscard]] PolicyId define_policy(RecoveryPolicy policy);
  [[nodiscard]] bool replace_policy(PolicyId id, RecoveryPolicy replacement);
  [[nodiscard]] const RecoveryPolicy* policy(PolicyId id) const;
  [[nodiscard]] std::vector<PolicyId> policy_ids() const;

  // ---- plans -------------------------------------------------------------
  [[nodiscard]] PlanHandle create_plan(const CreatePlanRequest& request);
  [[nodiscard]] bool has_plan(RecoveryPlanId id) const;
  [[nodiscard]] const RecoveryPlan* plan(RecoveryPlanId id) const;
  [[nodiscard]] std::vector<RecoveryPlanId> plan_ids() const;
  bool erase_plan(RecoveryPlanId id);

  // ---- epoch / authority -------------------------------------------------
  [[nodiscard]] CoordinatorEpoch epoch() const;
  CoordinatorEpoch begin_epoch(CoordinatorEpoch requested);
  bool grant_authority(const RecoveryAuthority& grant);
  bool revoke_authority(ResourceId resource);
  std::size_t drop_stale_authority(std::uint64_t now_tick);
  [[nodiscard]] AuthorityVector authority_snapshot() const;

  // ---- transitions -------------------------------------------------------
  // evaluate() is pure: it never mutates authoritative state.
  [[nodiscard]] StageDecision evaluate(RecoveryPlanId id, PlanGeneration generation,
                                       const EvidenceSnapshot& snapshot) const;
  [[nodiscard]] StageDecision advance(RecoveryPlanId id, PlanGeneration generation,
                                      const EvidenceSnapshot& snapshot);
  [[nodiscard]] CompletionOutcome complete(RecoveryPlanId id, PlanGeneration generation,
                                           const EvidenceSnapshot& snapshot);
  [[nodiscard]] StageDecision pause(RecoveryPlanId id, PlanGeneration generation, RejectReason reason,
                                    std::string text);
  [[nodiscard]] StageDecision resume(RecoveryPlanId id, PlanGeneration generation, std::uint64_t now_tick);
  [[nodiscard]] RollbackOutcome rollback(const RollbackRequest& request);

  // ---- invalidation ------------------------------------------------------
  // Returns the number of plans pushed into REVALIDATION_REQUIRED.
  std::size_t notify_resource_change(ResourceId resource, ResourceGeneration generation);
  std::size_t notify_policy_change(PolicyId id, PolicyGeneration generation);
  // Declares that an in-flight attempt's outcome is ambiguous (for example the
  // worker applying it died). The plan must revalidate before advancing again;
  // the attempt is never assumed to have succeeded.
  bool require_revalidation(RecoveryPlanId id, RejectReason reason, std::string detail);

  [[nodiscard]] RevalidationOutcome revalidate(RecoveryPlanId id, PlanGeneration generation,
                                               PolicyGeneration policy_generation,
                                               const std::vector<ResourceBinding>& resources,
                                               std::uint64_t now_tick);

  // ---- explanation / inspection -----------------------------------------
  [[nodiscard]] bool explain(RecoveryPlanId id, RecoveryExplanation& out) const;
  [[nodiscard]] std::string render_explanation(RecoveryPlanId id) const;

  // ---- durability --------------------------------------------------------
  [[nodiscard]] DurableState export_state() const;
  RestoreReport import_state(const DurableState& state, CoordinatorEpoch new_epoch);
  [[nodiscard]] std::vector<LineageEvent> drain_lineage_events();

  // ---- counters ----------------------------------------------------------
  [[nodiscard]] std::uint64_t evaluations() const;
  [[nodiscard]] std::uint64_t advances() const;
  [[nodiscard]] std::uint64_t stale_rejections() const;
  [[nodiscard]] std::uint64_t completion_replays() const;
  [[nodiscard]] std::uint64_t rollbacks_performed() const;

  [[nodiscard]] const AdjacentRequestLog& adjacent_requests() const;
  void clear_adjacent_requests();

 private:
  struct PlanSlot;
  struct PolicySlot;

  [[nodiscard]] const PolicySlot* policy_slot_locked(PolicyId id) const noexcept;
  [[nodiscard]] PlanSlot* plan_slot_locked(RecoveryPlanId id) const noexcept;

  // Pure decision computation against a draft plan. Mutates only the draft.
  [[nodiscard]] StageDecision compute_decision_locked(RecoveryPlan& draft,
                                                      const EvidenceSnapshot& snapshot) const;
  [[nodiscard]] StageDecision evaluate_locked(RecoveryPlanId id, PlanGeneration generation,
                                              const EvidenceSnapshot& snapshot, bool apply) const;
  [[nodiscard]] bool evaluate_requirements_locked(const RecoveryPlan& plan, const RecoveryPolicy& policy,
                                                  const EvidenceSnapshot& snapshot,
                                                  std::vector<EvidenceEvaluation>& evaluations,
                                                  bool& contradictory, RejectReason& reason,
                                                  std::string& detail) const;
  [[nodiscard]] bool build_candidate_locked(const RecoveryPlan& plan, const StageSpec& stage,
                                            std::uint64_t now_tick, RestorationVector& candidate,
                                            std::vector<ObligationEvaluation>& obligations,
                                            std::vector<AuthorityView>& authority, RejectReason& reason,
                                            std::string& detail, bool& clamped_authority) const;
  [[nodiscard]] RestorationVector collect_observed_locked(const RecoveryPlan& plan,
                                                          const RecoveryPolicy& policy,
                                                          const EvidenceSnapshot& snapshot,
                                                          const RestorationVector& candidate) const;
  void invalidate_plan_locked(PlanSlot& slot, RejectReason reason, const std::string& detail);
  void emit_locked(const std::string& kind, const RecoveryPlan& plan, const std::string& detail);
  [[nodiscard]] bool plan_obligations_hold(const RecoveryPlan& plan, const RestorationVector& levels,
                                           std::string& detail) const;

  EngineLimits limits_{};
  mutable std::mutex mutex_{};
  CoordinatorEpoch epoch_{};
  std::unordered_map<std::uint64_t, std::unique_ptr<PolicySlot>> policies_{};
  std::unordered_map<std::uint64_t, std::unique_ptr<PlanSlot>> plans_{};
  AuthorityVector authority_{};
  AdjacentRequestLog adjacent_{EngineLimits{}.max_adjacent_requests};
  std::vector<LineageEvent> lineage_{};
  std::uint64_t next_policy_id_{1};
  std::uint64_t next_plan_id_{1};
  std::uint64_t evaluations_{0};
  std::uint64_t advances_{0};
  std::uint64_t stale_rejections_{0};
  std::uint64_t completion_replays_{0};
  std::uint64_t rollbacks_performed_{0};
};

}  // namespace congestion_recovery
