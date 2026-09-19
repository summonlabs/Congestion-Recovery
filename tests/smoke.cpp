// Congestion Recovery - end-to-end staged restoration smoke suite.
// Copyright 2026 Summon Software Labs.
#include <string>

#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

void test_policy_validation() {
  SECTION("policy validation");
  RecoveryEngine engine{EngineLimits{}};
  RecoveryPolicy good = make_policy();
  CHK(engine.define_policy(good).is_valid());

  RecoveryPolicy no_complete = make_policy();
  no_complete.generation = PolicyGeneration(2);
  no_complete.stages.pop_back();
  CHK(!engine.define_policy(no_complete).is_valid());

  RecoveryPolicy descending = make_policy();
  descending.generation = PolicyGeneration(3);
  std::swap(descending.stages[1], descending.stages[2]);
  CHK(!engine.define_policy(descending).is_valid());

  RecoveryPolicy no_primary = make_policy();
  no_primary.generation = PolicyGeneration(4);
  no_primary.stages[2].requirements[1].primary = false;
  CHK(!engine.define_policy(no_primary).is_valid());

  RecoveryPolicy bad_margin = make_policy();
  bad_margin.generation = PolicyGeneration(5);
  bad_margin.hysteresis.rollback_margin = 5000;
  CHK(!engine.define_policy(bad_margin).is_valid());

  RecoveryPolicy observe_with_authority = make_policy();
  observe_with_authority.generation = PolicyGeneration(6);
  observe_with_authority.stages[0].requires_authority = true;
  CHK(!engine.define_policy(observe_with_authority).is_valid());

  RecoveryPolicy stage_jumps_levels = make_policy();
  stage_jumps_levels.generation = PolicyGeneration(7);
  (void)stage_jumps_levels.stages[3].target.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, 900);
  stage_jumps_levels.stages[3].requires_authority = false;
  CHK(!engine.define_policy(stage_jumps_levels).is_valid());

  RecoveryPolicy duplicate_id = make_policy();
  duplicate_id.id = PolicyId(1);
  duplicate_id.generation = PolicyGeneration(1);
  CHK(!engine.define_policy(duplicate_id).is_valid());
}

void test_plan_creation_validation() {
  SECTION("plan creation validation");
  Fixture fx;
  CHK(fx.build());
  CHK(fx.engine.plan(fx.handle.id) != nullptr);

  // OBSERVE target must equal the constrained state.
  CreatePlanRequest bad = CreatePlanRequest{};
  bad.policy_id = fx.policy;
  bad.policy_generation = fx.policy_generation;
  bad.congestion = CongestionRefId(1);
  bad.intervention = InterventionId(1);
  bad.epoch = fx.epoch;
  bad.now_tick = 1000;
  bad.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
  bad.resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
  bad.baseline = make_levels(kBaselineR1, kBaselineR2);
  bad.baseline_known = true;
  bad.constrained = make_levels(111, 222);
  bad.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);
  CHK(!fx.engine.create_plan(bad).is_valid());

  // Missing resource binding.
  CreatePlanRequest missing = bad;
  missing.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  missing.resources.clear();
  missing.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
  CHK(!fx.engine.create_plan(missing).is_valid());

  // Duplicate resource binding.
  CreatePlanRequest dup = bad;
  dup.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  dup.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
  CHK(!fx.engine.create_plan(dup).is_valid());

  // Stale epoch.
  CreatePlanRequest stale = bad;
  stale.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  stale.resources.clear();
  stale.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
  stale.resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
  stale.epoch = CoordinatorEpoch(99);
  CHK(!fx.engine.create_plan(stale).is_valid());

  // Rollback target above the constrained state is rejected.
  CreatePlanRequest upward = stale;
  upward.epoch = fx.epoch;
  upward.rollback_target = make_levels(kBaselineR1, kBaselineR2);
  CHK(!fx.engine.create_plan(upward).is_valid());
}

void test_full_ladder() {
  SECTION("full staged ladder");
  Fixture fx;
  CHK(fx.build());
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::OBSERVING);
  CHK(p != nullptr && p->stage_index == 0);

  std::uint64_t tick = 1000;
  const char* expected[] = {"PROBING", "PARTIAL_RESTORE", "HOLDING", "EXPANDING", "VERIFYING"};
  for (std::size_t i = 0; i < 5; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    const StageDecision d = step(fx, tick, rate_for(i), true, &applied);
    CHK(d.advanced);
    CHK(d.decision == RecoveryDecision::ADVANCE_STAGE || d.decision == RecoveryDecision::COMPLETE);
    const RecoveryPlan* now = fx.engine.plan(fx.handle.id);
    CHK(now != nullptr);
    if (now != nullptr) {
      CHK(std::string(to_string(now->state)) == expected[i]);
    }
  }
  const RecoveryPlan* verifying = fx.engine.plan(fx.handle.id);
  CHK(verifying != nullptr && verifying->state == RecoveryState::VERIFYING);
  CHK(verifying != nullptr && verifying->completions.empty());

  // Occupying the COMPLETE stage is not completion: the applied restoration
  // must still be observed.
  tick += 20;
  const RestorationVector final_applied = target_for(4);
  const StageDecision finishing = step(fx, tick, rate_for(4), true, &final_applied);
  CHK(finishing.decision == RecoveryDecision::COMPLETE);
  CHK(finishing.completed);
  const RecoveryPlan* done = fx.engine.plan(fx.handle.id);
  CHK(done != nullptr && done->state == RecoveryState::COMPLETED);
  CHK(done != nullptr && done->completion_count == 1);
  CHK(done != nullptr && done->completions.size() == 1);
  CHK(done != nullptr && done->completions.front().state == CompletionState::COMMITTED);
  CHK(fx.engine.advances() == 6);

  // Authorized vector equals the final stage target.
  CHK(done != nullptr && done->authorized.effective_level(kR1) == kBaselineR1);
  CHK(done != nullptr && done->authorized.effective_level(kR2) == kBaselineR2);

  // Terminal plan no longer advances.
  const StageDecision after = step(fx, tick + 20, 3000, true, nullptr);
  CHK(!after.advanced);
  CHK(after.decision == RecoveryDecision::NO_OP);
  CHK(after.reason == RejectReason::NOT_LIVE);
}

void test_explanation_surface() {
  SECTION("explanation surface");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  for (std::size_t i = 0; i < 2; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    CHK(step(fx, tick, rate_for(i), true, &applied).advanced);
  }
  RecoveryExplanation ex{};
  CHK(fx.engine.explain(fx.handle.id, ex));
  CHK(ex.state == RecoveryState::PARTIAL_RESTORE);
  CHK(ex.stage_kind == StageKind::PARTIAL_RESTORE);
  CHK(ex.stage_count == 6);
  CHK(ex.restoration.size() == 2);
  CHK(ex.required_evidence.size() == 2);
  CHK(ex.authority.size() == 2);
  CHK(ex.authority[0].present);
  CHK(!ex.revalidation_required);
  const std::string rendered = fx.engine.render_explanation(fx.handle.id);
  CHK(rendered.find("state=PARTIAL_RESTORE") != std::string::npos);
  CHK(rendered.find("restoration resource=1001") != std::string::npos);
  CHK(rendered.find("authority resource=1001") != std::string::npos);
  std::printf("%s", rendered.c_str());
  RecoveryExplanation missing{};
  CHK(!fx.engine.explain(RecoveryPlanId(4242), missing));
}

void test_evaluate_is_pure() {
  SECTION("evaluate never mutates");
  Fixture fx;
  CHK(fx.build());
  const RecoveryPlan before = *fx.engine.plan(fx.handle.id);
  std::uint64_t tick = 1020;
  const RestorationVector applied = target_for(0);
  const EvidenceSnapshot s = fx.snapshot(tick, rate_for(0), true, &applied);
  const StageDecision d = fx.engine.evaluate(fx.handle.id, fx.handle.generation, s);
  CHK(d.decision == RecoveryDecision::ADVANCE_STAGE);
  const RecoveryPlan after = *fx.engine.plan(fx.handle.id);
  CHK(before.stage_index == after.stage_index);
  CHK(before.stage_generation == after.stage_generation);
  CHK(before.total_attempts == after.total_attempts);
  CHK(before.stage_history.size() == after.stage_history.size());
  CHK(before.updated_at_tick == after.updated_at_tick);
  CHK(fx.engine.evaluations() == 0);
}

void test_epoch_and_authority() {
  SECTION("epoch and authority");
  Fixture fx;
  CHK(fx.build());
  CHK(fx.engine.epoch() == CoordinatorEpoch(1));
  CHK(fx.engine.authority_snapshot().size() == 2);
  // Epochs never regress.
  CHK(fx.engine.begin_epoch(CoordinatorEpoch(1)) == CoordinatorEpoch(1));
  CHK(fx.engine.begin_epoch(CoordinatorEpoch(0)) == CoordinatorEpoch(1));
  CHK(fx.engine.begin_epoch(CoordinatorEpoch(2)) == CoordinatorEpoch(2));
  CHK(fx.engine.authority_snapshot().size() == 0);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::REVALIDATION_REQUIRED);
  CHK(p != nullptr && p->revalidation_required);
  // A grant from the old epoch is refused.
  RecoveryAuthority old{};
  old.id = AuthorityId(50);
  old.generation = AuthorityGeneration(50);
  old.epoch = CoordinatorEpoch(1);
  old.resource = kR1;
  old.unit = RestorationUnit::BYTES_PER_SECOND;
  old.ceiling = 1000;
  old.expires_at_tick = 100000;
  CHK(!fx.engine.grant_authority(old));
  old.epoch = CoordinatorEpoch(2);
  CHK(fx.engine.grant_authority(old));
  // Generation regression on the same resource is refused.
  RecoveryAuthority regress = old;
  regress.generation = AuthorityGeneration(49);
  CHK(!fx.engine.grant_authority(regress));
}

void test_stale_evidence_rejection() {
  SECTION("stale evidence cannot advance");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1020;
  const RestorationVector applied = target_for(0);

  // Snapshot bound to a foreign plan generation.
  EvidenceSnapshot foreign = fx.snapshot(tick, rate_for(0), true, &applied);
  foreign.plan_generation = PlanGeneration(999);
  StageDecision d = fx.engine.advance(fx.handle.id, fx.handle.generation, foreign);
  CHK(!d.advanced && d.reason == RejectReason::STALE_PLAN);

  // Snapshot bound to a foreign epoch.
  EvidenceSnapshot wrong_epoch = fx.snapshot(tick, rate_for(0), true, &applied);
  wrong_epoch.epoch = CoordinatorEpoch(42);
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, wrong_epoch);
  CHK(!d.advanced && d.reason == RejectReason::STALE_EPOCH);

  // Evidence older than the freshness window.
  EvidenceSnapshot old = fx.snapshot(tick, rate_for(0), true, &applied);
  for (Evidence& e : old.entries) {
    e.observed_at_tick = tick - 5000;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, old);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::REJECT_EVIDENCE && d.reason == RejectReason::EVIDENCE_INSUFFICIENT);

  // Evidence observed in the future proves nothing.
  EvidenceSnapshot future = fx.snapshot(tick, rate_for(0), true, &applied);
  for (Evidence& e : future.entries) {
    e.observed_at_tick = tick + 100;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, future);
  CHK(!d.advanced && d.decision == RecoveryDecision::REJECT_EVIDENCE);

  // Snapshot from before the plan's current state.
  EvidenceSnapshot past = fx.snapshot(tick, rate_for(0), true, &applied);
  past.assembled_at_tick = 900;
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, past);
  CHK(!d.advanced && d.reason == RejectReason::STALE_EVIDENCE);

  // Wrong plan generation handle.
  d = fx.engine.advance(fx.handle.id, PlanGeneration(9), foreign);
  CHK(!d.advanced && d.reason == RejectReason::GENERATION_MISMATCH);

  // Unknown plan.
  d = fx.engine.advance(RecoveryPlanId(9999), PlanGeneration(1), foreign);
  CHK(!d.advanced && d.reason == RejectReason::NOT_FOUND);

  // Provenance of UNKNOWN never satisfies a requirement.
  EvidenceSnapshot unknown = fx.snapshot(tick, rate_for(0), true, &applied);
  for (Evidence& e : unknown.entries) {
    e.provenance = Provenance::UNKNOWN;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, unknown);
  CHK(!d.advanced && d.decision == RecoveryDecision::REJECT_EVIDENCE);
}

}  // namespace

int main() {
  crtest::init();
  test_policy_validation();
  test_plan_creation_validation();
  test_full_ladder();
  test_explanation_surface();
  test_evaluate_is_pure();
  test_epoch_and_authority();
  test_stale_evidence_rejection();
  return summary("cr_smoke");
}
