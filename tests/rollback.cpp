// Congestion Recovery - compensating rollback and completion lineage suite.
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

RollbackRequest make_request(const Fixture& fx, const RestorationVector& observed, std::uint64_t tick,
                             Provenance provenance = Provenance::MEASURED) {
  RollbackRequest request{};
  request.plan = fx.handle.id;
  request.plan_generation = fx.engine.plan(fx.handle.id)->generation;
  request.epoch = fx.epoch;
  request.observed_current = observed;
  request.provenance = provenance;
  request.evidence_generation = EvidenceGeneration(1);
  request.now_tick = tick;
  request.trigger = RejectReason::RECURRENCE_DETECTED;
  request.reason = "congestion recurred during staged restoration";
  return request;
}

void test_compensation_recomputed_from_observed() {
  SECTION("compensating plan recomputed from observed state");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);

  // The plan authorised 400/500; the operator observes that the full requested
  // restoration was applied. Compensation must be measured from what was seen.
  const RollbackOutcome outcome = fx.engine.rollback(make_request(fx, make_observed(400, 500), tick + 20));
  CHK(outcome.ok);
  CHK(outcome.compensation.complete);
  CHK(outcome.compensation.any_compensation);
  CHK(outcome.compensation.steps.size() == 2);
  CHK(outcome.state_after == RecoveryState::ROLLED_BACK);
  std::int64_t total = 0;
  for (const RestorationDelta& step_delta : outcome.compensation.steps) {
    CHK(step_delta.known && !step_delta.overflow);
    total += step_delta.delta;
    if (step_delta.resource == kR1) {
      CHK(step_delta.delta == -200);
    }
    if (step_delta.resource == kR2) {
      CHK(step_delta.delta == -100);
    }
  }
  CHK(total == -300);

  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->rollback_history.size() == 1);
  CHK(p != nullptr && p->rollback_history.front().compensation_steps == 2);
  CHK(p != nullptr && p->authorized.effective_level(kR1) == kConstrainedR1);
  CHK(p != nullptr && p->authorized.effective_level(kR2) == kConstrainedR2);

  // Rollback is not a blind vector reversal: it never restores upward.
  for (const RestorationDelta& step_delta : outcome.compensation.steps) {
    CHK(step_delta.delta <= 0);
  }
}

void test_unobserved_state_is_not_guessed() {
  SECTION("unobserved applied state is reported, never guessed");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);

  // Only R1 was observed. R2 must be listed as unobserved instead of assumed.
  RestorationVector partial = make_observed(400, 500);
  partial.find_mut(kR2)->observed_known = false;
  const RollbackOutcome outcome = fx.engine.rollback(make_request(fx, partial, tick + 20));
  CHK(outcome.ok);
  CHK(!outcome.compensation.complete);
  CHK(!outcome.compensation.unobserved.empty());
  bool r2_unobserved = false;
  for (const ResourceId id : outcome.compensation.unobserved) {
    if (id == kR2) {
      r2_unobserved = true;
    }
  }
  CHK(r2_unobserved);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->rollback_history.front().kind == RollbackKind::PARTIAL_COMPENSATION);
  CHK(p != nullptr && p->rollback_history.front().assumed_steps >= 1);
}

void test_rollback_requires_real_observation() {
  SECTION("rollback requires a real observation");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 1, tick);
  RollbackRequest request = make_request(fx, make_observed(400, 500), tick + 20, Provenance::SYNTHETIC);
  RollbackOutcome outcome = fx.engine.rollback(request);
  CHK(!outcome.ok && outcome.reason == RejectReason::EVIDENCE_INSUFFICIENT);
  request.provenance = Provenance::UNKNOWN;
  outcome = fx.engine.rollback(request);
  CHK(!outcome.ok);
  request.provenance = Provenance::REPORTED;
  outcome = fx.engine.rollback(request);
  CHK(outcome.ok);

  // A stale epoch is refused outright.
  request.epoch = CoordinatorEpoch(9);
  request.plan_generation = fx.engine.plan(fx.handle.id)->generation;
  const RollbackOutcome stale = fx.engine.rollback(request);
  CHK(!stale.ok && stale.reason == RejectReason::STALE_EPOCH);

  // A structurally invalid observed vector is refused.
  request.epoch = fx.epoch;
  RestorationVector broken{};
  (void)broken.set_level(ResourceId{}, RestorationUnit::UNKNOWN, 5);
  request.observed_current = broken;
  const RollbackOutcome invalid = fx.engine.rollback(request);
  CHK(!invalid.ok && invalid.reason == RejectReason::INVALID_INPUT);
}

void test_rollback_budget() {
  SECTION("rollback budget is bounded");
  LadderConfig cfg;
  cfg.max_rollbacks = 3;
  cfg.max_transitions = 20;
  Fixture fx;
  CHK(fx.build(cfg));
  std::uint64_t tick = 1000;
  advance_to(fx, 1, tick);

  for (int round = 0; round < 2; ++round) {
    const RestorationVector applied = target_for(1);
    const StageDecision d = step(fx, tick + 20, 100, true, &applied);
    CHK(d.decision == RecoveryDecision::ROLLBACK);
    tick += 40;
    const RollbackOutcome outcome = fx.engine.rollback(make_request(fx, make_observed(400, 500), tick));
    CHK(outcome.ok);
    CHK(!outcome.plan_failed);
    tick += 60;
    const std::size_t stage_now = fx.engine.plan(fx.handle.id)->stage_index;
    const StageDecision resumed = step(fx, tick, rate_for(stage_now), true, &applied);
    CHK(resumed.advanced);
    tick += 20;
  }

  // A third rollback exhausts the budget and fails the plan.
  const StageDecision d = step(fx, tick, 100, true, nullptr);
  CHK(d.decision == RecoveryDecision::ROLLBACK);
  tick += 20;
  const RollbackOutcome outcome = fx.engine.rollback(make_request(fx, make_observed(400, 500), tick));
  CHK(outcome.ok);
  CHK(outcome.plan_failed);
  CHK(outcome.state_after == RecoveryState::FAILED);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->rollback_count == 3);
  CHK(p != nullptr && p->last_reason == RejectReason::ROLLBACK_BUDGET_EXHAUSTED);
}

void test_completion_is_idempotent() {
  SECTION("duplicate completion is idempotent");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);
  const RestorationVector final_applied = target_for(4);
  const EvidenceSnapshot s = fx.snapshot(tick + 20, rate_for(4), true, &final_applied);

  const CompletionOutcome first = fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(first.ok);
  CHK(!first.duplicate);
  CHK(first.state == CompletionState::COMMITTED);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completion_count == 1);
  CHK(p != nullptr && p->completions.size() == 1);

  const CompletionOutcome replay = fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(replay.ok);
  CHK(replay.duplicate);
  CHK(replay.state == CompletionState::COMMITTED);
  CHK(replay.completion == first.completion);
  p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completion_count == 1);
  CHK(p != nullptr && p->completions.size() == 1);
  CHK(fx.engine.completion_replays() == 1);
}

void test_failed_completion_cannot_be_reused() {
  SECTION("failed completion can never be reused");
  LadderConfig cfg;
  cfg.complete_rollback_on_failure = false;  // hold and retry instead of rolling back
  Fixture fx;
  CHK(fx.build(cfg));
  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);

  const RestorationVector partial = make_levels(100, 200);
  const EvidenceSnapshot s = fx.snapshot(tick + 20, rate_for(4), true, &partial);
  const CompletionOutcome failed = fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(!failed.ok);
  CHK(failed.reason == RejectReason::PARTIAL_APPLICATION);
  CHK(failed.state == CompletionState::FAILED);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completions.size() == 1);
  CHK(p != nullptr && p->completions.front().state == CompletionState::FAILED);

  // Replaying the identical evidence returns the recorded failure and does not
  // append a second record or advance anything.
  const CompletionOutcome replay = fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(!replay.ok);
  CHK(replay.duplicate);
  CHK(replay.state == CompletionState::FAILED);
  p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completions.size() == 1);
  CHK(p != nullptr && p->state == RecoveryState::VERIFYING);

  // Fresh evidence is a new attempt, and it can succeed.
  const RestorationVector complete_applied = target_for(4);
  const EvidenceSnapshot good = fx.snapshot(tick + 40, rate_for(4), true, &complete_applied);
  const CompletionOutcome retried =
      fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, good);
  CHK(retried.ok);
  CHK(retried.state == CompletionState::COMMITTED);
  p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completions.size() == 2);
  CHK(p != nullptr && p->completion_count == 1);
}

void test_stale_completion_replay() {
  SECTION("stale completion replay is rejected");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);
  const RestorationVector final_applied = target_for(4);
  const EvidenceSnapshot s = fx.snapshot(tick + 20, rate_for(4), true, &final_applied);
  CHK(fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s).ok);

  // Replaying under an older plan generation is a stale completion, not a
  // duplicate.
  const CompletionOutcome stale = fx.engine.complete(fx.handle.id, PlanGeneration(0), s);
  CHK(!stale.ok && stale.reason == RejectReason::GENERATION_MISMATCH);
  CHK(stale.state == CompletionState::REJECTED_STALE);
  CHK(!stale.duplicate);

  // After an epoch advance the same frame is stale work, not an idempotent
  // replay: it must be refused without touching the committed completion.
  const CoordinatorEpoch next = fx.engine.begin_epoch(CoordinatorEpoch(2));
  CHK(next == CoordinatorEpoch(2));
  const CompletionOutcome stale_epoch =
      fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(!stale_epoch.ok);
  CHK(!stale_epoch.duplicate || stale_epoch.state != CompletionState::COMMITTED);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->completion_count == 1);
}

void test_completion_requires_terminal_stage() {
  SECTION("completion requires the terminal stage");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);
  const RestorationVector applied = target_for(2);
  const EvidenceSnapshot s = fx.snapshot(tick + 20, rate_for(2), true, &applied);
  const CompletionOutcome outcome =
      fx.engine.complete(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, s);
  CHK(!outcome.ok);
  CHK(outcome.reason == RejectReason::NOT_LIVE);
  CHK(fx.engine.plan(fx.handle.id)->completions.empty());
}

void test_rollback_then_recovery_completes() {
  SECTION("rollback then recovery completes");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 3, tick);

  // Recurrence rolls the plan back to the constrained envelope.
  const RestorationVector applied = target_for(3);
  CHK(step(fx, tick + 20, 10, true, &applied).decision == RecoveryDecision::ROLLBACK);
  tick += 40;
  const RollbackOutcome outcome = fx.engine.rollback(make_request(fx, make_observed(700, 650), tick));
  CHK(outcome.ok);
  CHK(outcome.compensation.steps.size() == 2);
  for (const RestorationDelta& step_delta : outcome.compensation.steps) {
    if (step_delta.resource == kR1) {
      CHK(step_delta.delta == -500);
    }
    if (step_delta.resource == kR2) {
      CHK(step_delta.delta == -250);
    }
  }
  tick += 60;
  for (std::size_t i = 3; i < 5; ++i) {
    const RestorationVector stage_applied = target_for(i);
    const StageDecision d = step(fx, tick + 20, rate_for(i), true, &stage_applied);
    CHK(d.advanced);
    tick += 20;
  }
  const RestorationVector final_applied = target_for(4);
  const StageDecision done = step(fx, tick + 20, rate_for(4), true, &final_applied);
  CHK(done.decision == RecoveryDecision::COMPLETE);
  const RecoveryPlan* p = fx.engine.plan(fx.handle.id);
  CHK(p != nullptr && p->state == RecoveryState::COMPLETED);
  CHK(p != nullptr && p->rollback_history.size() == 1);
  CHK(p != nullptr && p->stage_history.size() >= 6);
}

void test_adjacent_request_log_bounds() {
  SECTION("adjacent request log is bounded");
  AdjacentRequestLog log(4);
  for (std::uint64_t i = 1; i <= 10; ++i) {
    AdjacentRequest request{};
    request.kind = AdjacentRequestKind::REQUEST_PATH_REVALIDATION;
    request.plan = RecoveryPlanId(1);
    request.resource = ResourceId(i);
    request.amount = i;
    request.owner_runtime = "Path Authority";
    CHK(log.push(request));
  }
  CHK(log.size() == 4);
  CHK(log.dropped() == 6);
  const std::vector<std::string> rendered = log.render(10);
  CHK(rendered.size() == 4);
  CHK(rendered.front().find("REQUEST_PATH_REVALIDATION") != std::string::npos);
  AdjacentRequest invalid{};
  invalid.kind = AdjacentRequestKind::UNKNOWN;
  CHK(!log.push(invalid));
}

}  // namespace

int main() {
  crtest::init();
  test_compensation_recomputed_from_observed();
  test_unobserved_state_is_not_guessed();
  test_rollback_requires_real_observation();
  test_rollback_budget();
  test_completion_is_idempotent();
  test_failed_completion_cannot_be_reused();
  test_stale_completion_replay();
  test_completion_requires_terminal_stage();
  test_rollback_then_recovery_completes();
  test_adjacent_request_log_bounds();
  return summary("cr_rollback");
}
