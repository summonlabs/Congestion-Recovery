// Congestion Recovery - stage transition, hysteresis and authority suite.
// Copyright 2026 Summon Software Labs.
#include <string>

#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

void advance_to(Fixture& fx, std::size_t last_stage, std::uint64_t& tick) {
  for (std::size_t i = 0; i < last_stage; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    const StageDecision d = step(fx, tick, rate_for(i), true, &applied);
    CHK(d.advanced);
  }
}

void test_dwell_and_hold_band() {
  SECTION("dwell and hysteresis hold band");
  Fixture fx;
  CHK(fx.build());

  // Too early: dwell has not elapsed.
  const RestorationVector applied = target_for(0);
  StageDecision d = step(fx, 1005, rate_for(0), true, &applied);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::HOLD && d.reason == RejectReason::DWELL_NOT_SATISFIED);

  // Dwell satisfied but the primary value sits inside the hold band.
  d = step(fx, 1020, 600, true, &applied);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::HOLD && d.reason == RejectReason::HYSTERESIS_BAND);

  // Just below the advance floor is still the hold band, not recurrence.
  d = step(fx, 1020, 699, true, &applied);
  CHK(!d.advanced && d.reason == RejectReason::HYSTERESIS_BAND);

  // At the advance floor the stage moves.
  d = step(fx, 1020, 700, true, &applied);
  CHK(d.advanced && d.decision == RecoveryDecision::ADVANCE_STAGE);
}

void test_recurrence_actions() {
  SECTION("recurrence actions");
  {
    LadderConfig cfg;
    cfg.on_recurrence = RecurrenceAction::PAUSE;
    Fixture fx;
    CHK(fx.build(cfg));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, 100, true, &applied);
    CHK(!d.advanced);
    CHK(d.decision == RecoveryDecision::PAUSE);
    CHK(d.reason == RejectReason::RECURRENCE_DETECTED);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->state == RecoveryState::PAUSED);
  }
  {
    LadderConfig cfg;
    cfg.on_recurrence = RecurrenceAction::FAIL;
    Fixture fx;
    CHK(fx.build(cfg));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, 100, true, &applied);
    CHK(d.decision == RecoveryDecision::FAIL);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->state == RecoveryState::FAILED);
    // Terminal plans never advance again.
    const StageDecision again = step(fx, tick + 40, 5000, true, &applied);
    CHK(again.decision == RecoveryDecision::NO_OP && again.reason == RejectReason::NOT_LIVE);
  }
  {
    LadderConfig cfg;
    cfg.on_recurrence = RecurrenceAction::ROLLBACK;
    Fixture fx;
    CHK(fx.build(cfg));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, 100, true, &applied);
    CHK(d.decision == RecoveryDecision::ROLLBACK);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->state == RecoveryState::ROLLING_BACK);
    // While rolling back, no further stage work is accepted.
    const StageDecision blocked = step(fx, tick + 40, 5000, true, &applied);
    CHK(blocked.decision == RecoveryDecision::HOLD);
    CHK(blocked.reason == RejectReason::RECURRENCE_DETECTED);
  }
}

void test_cooldown_after_rollback() {
  SECTION("cooldown after rollback");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 1, tick);
  const RestorationVector applied = target_for(1);
  CHK(step(fx, tick + 20, 100, true, &applied).decision == RecoveryDecision::ROLLBACK);

  RollbackRequest request{};
  request.plan = fx.handle.id;
  request.plan_generation = fx.handle.generation;
  request.epoch = fx.epoch;
  request.observed_current = make_observed(400, 500);
  request.provenance = Provenance::MEASURED;
  request.now_tick = tick + 30;
  request.trigger = RejectReason::RECURRENCE_DETECTED;
  request.reason = "congestion recurred";
  const RollbackOutcome outcome = fx.engine.rollback(request);
  CHK(outcome.ok);
  CHK(outcome.state_after == RecoveryState::ROLLED_BACK);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->rollback_count == 1);

  // Cooldown blocks an immediate restart.
  const StageDecision d = step(fx, tick + 32, 5000, true, &applied);
  CHK(!d.advanced && d.reason == RejectReason::COOLDOWN_ACTIVE);

  // After the cooldown the plan resumes on the same stage.
  const StageDecision after = step(fx, tick + 60, rate_for(1), true, &applied);
  CHK(after.advanced);
  CHK(after.stage_index_before == 1 && after.stage_index_after == 2);
}

void test_transition_rate_bound() {
  SECTION("bounded transition rate");
  LadderConfig cfg;
  cfg.max_transitions = 2;
  Fixture fx;
  CHK(fx.build(cfg));
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);
  const RestorationVector applied = target_for(2);
  const StageDecision d = step(fx, tick + 20, rate_for(2), true, &applied);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::PAUSE);
  CHK(d.reason == RejectReason::TRANSITION_RATE_EXCEEDED);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::PAUSED);
}

void test_authority_limits() {
  SECTION("authority limits restoration");
  {
    Fixture fx;
    CHK(fx.build());
    CHK(fx.grant_authority(500, 600, 0, 1000000));
    std::uint64_t tick = 1000;
    advance_to(fx, 2, tick);
    const RestorationVector applied = target_for(2);
    const StageDecision d = step(fx, tick + 20, rate_for(2), true, &applied);
    CHK(d.advanced);
    CHK(d.clamped_by_authority);
    // Restoration never exceeds current legal authority.
    std::size_t clamped = 0;
    for (const RestorationAmount& a : d.effective.entries()) {
      if (a.effective > a.requested) {
        clamped += 100;
      }
      if (a.resource == kR1) {
        CHK(a.effective == 500);
        CHK(a.authority_clamped);
      }
      if (a.resource == kR2) {
        CHK(a.effective == 600);
      }
    }
    CHK(clamped == 0);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->authorized.effective_level(kR1) == 500);
  }
  {
    Fixture fx;
    CHK(fx.build());
    CHK(fx.engine.revoke_authority(kR2));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, rate_for(1), true, &applied);
    CHK(!d.advanced);
    CHK(d.decision == RecoveryDecision::REJECT_AUTHORITY);
    CHK(d.reason == RejectReason::AUTHORITY_MISSING);
  }
  {
    Fixture fx;
    CHK(fx.build());
    CHK(fx.grant_authority(kBaselineR1, kBaselineR2, 0, 1010));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, rate_for(1), true, &applied);
    CHK(!d.advanced);
    CHK(d.decision == RecoveryDecision::REJECT_AUTHORITY);
    CHK(d.reason == RejectReason::AUTHORITY_EXPIRED);
    CHK(fx.engine.drop_stale_authority(2000) == 2);
    CHK(fx.engine.authority_snapshot().empty());
  }
}

void test_pause_resume() {
  SECTION("pause and resume");
  Fixture fx;
  CHK(fx.build());
  const StageDecision paused = fx.engine.pause(fx.handle.id, fx.handle.generation,
                                               RejectReason::CAPACITY_DROP, "operator pause");
  CHK(paused.decision == RecoveryDecision::PAUSE);
  CHK(paused.state_after == RecoveryState::PAUSED);
  const RestorationVector applied = target_for(0);
  const StageDecision blocked = step(fx, 1020, rate_for(0), true, &applied);
  CHK(!blocked.advanced && blocked.reason == RejectReason::NOT_LIVE);
  const StageDecision resumed = fx.engine.resume(fx.handle.id, fx.handle.generation, 1030);
  CHK(resumed.decision == RecoveryDecision::RESUME);
  CHK(resumed.state_after == RecoveryState::OBSERVING);
  const StageDecision advanced = step(fx, 1050, rate_for(0), true, &applied);
  CHK(advanced.advanced);
  // A pause carrying a stale plan generation is refused, never applied.
  const StageDecision stale_pause = fx.engine.pause(fx.handle.id, PlanGeneration(9),
                                                    RejectReason::CAPACITY_DROP, "late");
  CHK(stale_pause.decision == RecoveryDecision::REJECT_STALE);
  CHK(stale_pause.reason == RejectReason::GENERATION_MISMATCH);
}

void test_invalidation_and_revalidation() {
  SECTION("invalidation and revalidation");
  {
    Fixture fx;
    CHK(fx.build());
    CHK(fx.engine.notify_resource_change(kR1, ResourceGeneration(9)) == 1);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->revalidation_required);
    CHK(p != nullptr && p->state == RecoveryState::REVALIDATION_REQUIRED);
    CHK(p != nullptr && p->last_reason == RejectReason::STALE_RESOURCE);
    // A generation regression is not a change.
    CHK(fx.engine.notify_resource_change(kR1, ResourceGeneration(1)) == 0);

    const RestorationVector applied = target_for(0);
    const StageDecision d = step(fx, 1020, rate_for(0), true, &applied);
    CHK(d.decision == RecoveryDecision::REVALIDATE);
    CHK(d.reason == RejectReason::REVALIDATION_BOUNDARY);

    std::vector<ResourceBinding> resources;
    resources.push_back(ResourceBinding{kR1, ResourceGeneration(9)});
    resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
    const RevalidationOutcome outcome =
        fx.engine.revalidate(fx.handle.id, fx.handle.generation, fx.policy_generation, resources, 1030);
    CHK(outcome.ok);
    const RecoveryPlan* after = fx.engine.plan(fx.handle.id);
    CHK(after != nullptr && !after->revalidation_required);
    CHK(after != nullptr && after->generation == PlanGeneration(2));
    // The old handle generation is now stale.
    const StageDecision stale = step(fx, 1050, rate_for(0), true, &applied);
    CHK(!stale.advanced && stale.reason == RejectReason::GENERATION_MISMATCH);

    const EvidenceSnapshot s = fx.snapshot(1060, rate_for(0), true, &applied);
    const StageDecision ok = fx.engine.advance(fx.handle.id, after->generation, s);
    CHK(ok.advanced);

    // A regressed resource binding is refused.
    std::vector<ResourceBinding> regressed;
    regressed.push_back(ResourceBinding{kR1, ResourceGeneration(1)});
    regressed.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
    const RevalidationOutcome refused =
        fx.engine.revalidate(fx.handle.id, after->generation, fx.policy_generation, regressed, 1070);
    CHK(!refused.ok && refused.reason == RejectReason::STALE_RESOURCE);
  }
  {
    Fixture fx;
    CHK(fx.build());
    CHK(fx.engine.notify_policy_change(fx.policy, PolicyGeneration(2)) == 1);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->revalidation_required);
    CHK(p != nullptr && p->last_reason == RejectReason::STALE_POLICY);
    // Replacing the policy with a regressed generation is refused.
    RecoveryPolicy replacement = make_policy();
    replacement.generation = PolicyGeneration(1);
    CHK(!fx.engine.replace_policy(fx.policy, replacement));
  }
}

void test_revalidation_boundary_and_lifetime() {
  SECTION("revalidation boundary and plan lifetime");
  {
    LadderConfig cfg;
    cfg.revalidation_boundary = 50;
    Fixture fx;
    CHK(fx.build(cfg));
    std::uint64_t tick = 1000;
    advance_to(fx, 1, tick);
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 60, rate_for(1), true, &applied);
    CHK(d.decision == RecoveryDecision::REVALIDATE);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->revalidation_required);
  }
  {
    LadderConfig cfg;
    cfg.max_plan_lifetime = 30;
    Fixture fx;
    CHK(fx.build(cfg));
    const RestorationVector applied = target_for(0);
    const StageDecision d = step(fx, 1040, rate_for(0), true, &applied);
    CHK(d.decision == RecoveryDecision::FAIL);
    CHK(d.reason == RejectReason::PLAN_LIFETIME_EXCEEDED);
    const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
    CHK(p != nullptr && p->state == RecoveryState::FAILED);
  }
}

void test_evidence_edge_cases() {
  SECTION("evidence edge cases");
  Fixture fx;
  CHK(fx.build());
  const RestorationVector applied = target_for(0);

  // Missing affirmation: requirements are not met, so the stage holds.
  StageDecision d = step(fx, 1020, rate_for(0), false, &applied);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::HOLD && d.reason == RejectReason::EVIDENCE_INSUFFICIENT);

  // Not enough samples.
  EvidenceSnapshot weak = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : weak.entries) {
    e.sample_count = 1;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, weak);
  CHK(!d.advanced && d.decision == RecoveryDecision::HOLD);

  // Unstable evidence fails a stability requirement.
  EvidenceSnapshot unstable = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : unstable.entries) {
    e.stable = false;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, unstable);
  CHK(!d.advanced && d.decision == RecoveryDecision::HOLD);

  // Conflicting values under one evidence generation are contradictory.
  EvidenceSnapshot conflicting = fx.snapshot(1020, rate_for(0), true, &applied);
  Evidence twin = conflicting.entries[1];
  twin.value = 42;
  conflicting.entries.push_back(twin);
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, conflicting);
  CHK(!d.advanced);
  CHK(d.decision == RecoveryDecision::REJECT_EVIDENCE);
  CHK(d.reason == RejectReason::EVIDENCE_CONTRADICTORY);
}

void test_attempt_budget() {
  SECTION("attempt budget on the terminal stage");
  LadderConfig cfg;
  cfg.max_attempts = 2;
  cfg.complete_rollback_on_failure = false;
  Fixture fx;
  CHK(fx.build(cfg));

  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::VERIFYING);

  const RestorationVector partial = make_levels(100, 200);
  StageDecision d = step(fx, tick + 20, rate_for(4), true, &partial);
  CHK(!d.advanced && d.reason == RejectReason::PARTIAL_APPLICATION);
  p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->attempts_in_stage == 2);

  d = step(fx, tick + 40, rate_for(4), true, &partial);
  CHK(d.decision == RecoveryDecision::FAIL);
  CHK(d.reason == RejectReason::ATTEMPT_BUDGET_EXHAUSTED);
  p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::FAILED);
}

}  // namespace

int main() {
  crtest::init();
  test_dwell_and_hold_band();
  test_recurrence_actions();
  test_cooldown_after_rollback();
  test_transition_rate_bound();
  test_authority_limits();
  test_pause_resume();
  test_invalidation_and_revalidation();
  test_revalidation_boundary_and_lifetime();
  test_evidence_edge_cases();
  test_attempt_budget();
  return summary("cr_stages");
}
