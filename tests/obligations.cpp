// Congestion Recovery - protected obligation suite.
// Copyright 2026 Summon Software Labs.
#include <string>

#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

ProtectedObligation obligation(std::uint64_t id, ObligationKind kind, std::uint64_t threshold, bool hard,
                               bool active = true) {
  ProtectedObligation o{};
  o.id = ObligationId(id);
  o.generation = ObligationGeneration(1);
  o.kind = kind;
  o.resource = kR1;
  o.unit = RestorationUnit::BYTES_PER_SECOND;
  o.threshold = threshold;
  o.hard = hard;
  o.active = active;
  o.description = "test obligation";
  return o;
}

CreatePlanRequest base_request(const Fixture& fx) {
  CreatePlanRequest request{};
  request.policy_id = fx.policy;
  request.policy_generation = fx.policy_generation;
  request.congestion = CongestionRefId(5);
  request.intervention = InterventionId(6);
  request.epoch = fx.epoch;
  request.now_tick = 1000;
  request.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
  request.resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
  request.baseline = make_levels(kBaselineR1, kBaselineR2);
  request.baseline_known = true;
  request.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  request.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);
  return request;
}

void test_floor_obligation_blocks_creation() {
  SECTION("hard floor obligation blocks an unsafe plan");
  Fixture fx;
  CHK(fx.build());
  CreatePlanRequest request = base_request(fx);
  // The rollback target (200) cannot satisfy a floor of 300.
  request.obligations.push_back(obligation(1, ObligationKind::MIN_SERVICE_RATE, 300, true));
  CHK(!fx.engine.create_plan(request).is_valid());

  // A floor the rollback target does satisfy is accepted.
  request.obligations.clear();
  request.obligations.push_back(obligation(1, ObligationKind::MIN_SERVICE_RATE, 200, true));
  CHK(fx.engine.create_plan(request).is_valid());
  CHK(fx.engine.plan_ids().size() == 2);
}

StageDecision step_plan(Fixture& fx, RecoveryPlanId id, PlanGeneration generation, std::uint64_t tick,
                        std::uint64_t rate, const RestorationVector& applied) {
  const EvidenceSnapshot snapshot = fx.snapshot(tick, rate, true, &applied);
  return fx.engine.advance(id, generation, snapshot);
}

void test_ceiling_obligation_blocks_advance() {
  SECTION("hard ceiling obligation blocks a stage");
  Fixture fx;
  CHK(fx.build());
  CreatePlanRequest request = base_request(fx);
  // Constrained (200) and the rollback target (200) satisfy a ceiling of 500;
  // PARTIAL_RESTORE (700) does not.
  request.obligations.push_back(obligation(1, ObligationKind::MAX_QUEUE_DEPTH, 500, true));
  const PlanHandle handle = fx.engine.create_plan(request);
  CHK(handle.is_valid());

  std::uint64_t tick = 1000;
  const RestorationVector applied0 = target_for(0);
  tick += 20;
  CHK(step_plan(fx, handle.id, handle.generation, tick, rate_for(0), applied0).advanced);
  const RestorationVector applied1 = target_for(1);
  tick += 20;
  CHK(step_plan(fx, handle.id, handle.generation, tick, rate_for(1), applied1).advanced);

  // Stage 2 requests 700 on R1, above the obligation ceiling.
  const RestorationVector applied2 = target_for(2);
  tick += 20;
  const StageDecision d = step_plan(fx, handle.id, handle.generation, tick, rate_for(2), applied2);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::REJECT_OBLIGATION);
  CHK(d.reason == RejectReason::OBLIGATION_VIOLATION);
  CHK(!d.obligations.empty() && d.obligations.front().blocking);
  const RecoveryPlan* plan = fx.engine.plan(handle.id);
  CHK(plan != nullptr && plan->stage_index == 2);
  CHK(plan != nullptr && plan->state == RecoveryState::PARTIAL_RESTORE);
}

void test_soft_obligation_does_not_block() {
  SECTION("soft obligation is reported but does not block");
  Fixture fx;
  CHK(fx.build());
  CreatePlanRequest request = base_request(fx);
  request.obligations.push_back(obligation(1, ObligationKind::MAX_QUEUE_DEPTH, 500, false));
  const PlanHandle handle = fx.engine.create_plan(request);
  CHK(handle.is_valid());

  std::uint64_t tick = 1000;
  for (std::size_t i = 0; i < 3; ++i) {
    const RestorationVector applied = target_for(i);
    tick += 20;
    const StageDecision d = step_plan(fx, handle.id, handle.generation, tick, rate_for(i), applied);
    CHK(d.advanced);
  }
  RecoveryExplanation explanation{};
  CHK(fx.engine.explain(handle.id, explanation));
  CHK(!explanation.obligations.empty());
  CHK(explanation.obligations.front().state == ObligationState::VIOLATED);
  CHK(!explanation.obligations.front().blocking);
  CHK(!explanation.obligations.front().hard);
}

void test_inactive_and_unknown_obligations() {
  SECTION("inactive and unknown obligations");
  Fixture fx;
  CHK(fx.build());
  const RestorationVector candidate = make_levels(200, 400);
  ProtectedObligation inactive = obligation(1, ObligationKind::MIN_SERVICE_RATE, 5000, true, false);
  const ObligationEvaluation inactive_eval = evaluate_obligation(inactive, candidate);
  CHK(inactive_eval.state == ObligationState::NOT_EVALUATED);
  CHK(!inactive_eval.blocking);

  ProtectedObligation unknown_kind = obligation(2, ObligationKind::UNKNOWN, 10, true);
  const ObligationEvaluation unknown_eval = evaluate_obligation(unknown_kind, candidate);
  CHK(unknown_eval.state == ObligationState::NOT_EVALUATED);
  CHK(unknown_eval.blocking);

  ProtectedObligation absent_resource = obligation(3, ObligationKind::MIN_SERVICE_RATE, 10, true);
  absent_resource.resource = ResourceId(999);
  const ObligationEvaluation absent_eval = evaluate_obligation(absent_resource, candidate);
  CHK(absent_eval.state == ObligationState::NOT_EVALUATED);
  CHK(absent_eval.blocking);

  // An obligation bound to a resource outside the plan is rejected outright.
  CreatePlanRequest request = base_request(fx);
  ProtectedObligation foreign = obligation(4, ObligationKind::MIN_SERVICE_RATE, 10, true);
  foreign.resource = ResourceId(4242);
  request.obligations.push_back(foreign);
  CHK(!fx.engine.create_plan(request).is_valid());
}

void test_obligation_survives_rollback() {
  SECTION("protected obligations survive rollback");
  Fixture fx;
  CHK(fx.build());
  CreatePlanRequest request = base_request(fx);
  request.obligations.push_back(obligation(1, ObligationKind::MIN_SERVICE_RATE, 200, true));
  const PlanHandle handle = fx.engine.create_plan(request);
  CHK(handle.is_valid());

  std::uint64_t tick = 1000;
  for (std::size_t i = 0; i < 2; ++i) {
    const RestorationVector applied = target_for(i);
    tick += 20;
    CHK(step_plan(fx, handle.id, handle.generation, tick, rate_for(i), applied).advanced);
  }
  RollbackRequest rollback{};
  rollback.plan = handle.id;
  rollback.plan_generation = fx.engine.plan(handle.id)->generation;
  rollback.epoch = fx.epoch;
  rollback.observed_current = make_observed(400, 500);
  rollback.provenance = Provenance::MEASURED;
  rollback.now_tick = tick + 20;
  rollback.reason = "recurrence";
  const RollbackOutcome outcome = fx.engine.rollback(rollback);
  CHK(outcome.ok);
  // The compensating target is the obligation-safe rollback target.
  CHK(outcome.compensation.target.effective_level(kR1) == kConstrainedR1);
  const RecoveryPlan* plan = fx.engine.plan(handle.id);
  CHK(plan != nullptr && plan->authorized.effective_level(kR1) >= 200);
}

}  // namespace

int main() {
  crtest::init();
  test_floor_obligation_blocks_creation();
  test_ceiling_obligation_blocks_advance();
  test_soft_obligation_does_not_block();
  test_inactive_and_unknown_obligations();
  test_obligation_survives_rollback();
  return summary("cr_obligations");
}
