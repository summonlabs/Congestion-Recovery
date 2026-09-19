// Congestion Recovery - seeded randomized property suite.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

// splitmix64: deterministic across platforms and standard libraries.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) noexcept { return bound == 0u ? 0u : next() % bound; }

  bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

struct AuthorityCeiling {
  std::uint64_t r1{0};
  std::uint64_t r2{0};
};

LadderConfig random_config(Rng& rng) {
  LadderConfig cfg{};
  cfg.dwell = rng.below(25);
  cfg.advance_margin = rng.below(400);
  cfg.rollback_margin = rng.below(cfg.advance_margin + 1u);
  if (cfg.rollback_margin >= 600u) {
    cfg.rollback_margin = 100u;
  }
  cfg.cooldown = rng.below(20);
  cfg.max_transitions = static_cast<std::uint32_t>(2u + rng.below(30));
  cfg.window = 10000u;
  cfg.max_attempts = static_cast<std::uint32_t>(1u + rng.below(4));
  cfg.max_rollbacks = static_cast<std::uint32_t>(1u + rng.below(3));
  cfg.max_total_attempts = static_cast<std::uint32_t>(8u + rng.below(40));
  cfg.on_recurrence = static_cast<RecurrenceAction>(1u + rng.below(3));
  cfg.complete_rollback_on_failure = rng.chance(1u, 2u);
  return cfg;
}

void check_invariants(const Fixture& fx, const AuthorityCeiling& ceiling, std::size_t& highest_stage) {
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  CHK(plan != nullptr);
  if (plan == nullptr) {
    return;
  }
  CHK(is_valid(plan->state));
  CHK(plan->stage_index < 6u);
  CHK(plan->stage_index >= highest_stage);
  highest_stage = plan->stage_index;
  CHK(plan->rollback_count <= 3u);
  CHK(plan->total_attempts <= 200u);
  // Restoration never exceeds current legal authority. The OBSERVE rung records
  // the constrained state rather than restoring anything, so the bound there is
  // the constrained level itself.
  for (const RestorationAmount& amount : plan->authorized.entries()) {
    const std::uint64_t limit = amount.resource == kR1 ? ceiling.r1 : ceiling.r2;
    const std::uint64_t constrained =
        amount.resource == kR1 ? kConstrainedR1 : kConstrainedR2;
    CHK(amount.effective <= max_u64(limit, constrained));
  }
  // Completion lineage never repeats a key.
  std::set<std::uint64_t> keys;
  for (const CompletionRecord& record : plan->completions) {
    CHK(keys.insert(record.idempotency_key).second);
  }
  std::size_t committed = 0;
  for (const CompletionRecord& record : plan->completions) {
    if (record.state == CompletionState::COMMITTED) {
      ++committed;
    }
  }
  CHK(committed <= 1u);
  CHK(plan->completion_count == committed);
}

void random_walk(std::uint64_t seed) {
  Rng rng(seed);
  const LadderConfig cfg = random_config(rng);
  Fixture fx;
  if (!fx.build(cfg)) {
    return;
  }
  const std::uint64_t ceiling_r1 = 300u + rng.below(700u);
  const std::uint64_t ceiling_r2 = 300u + rng.below(500u);
  const AuthorityCeiling ceiling{ceiling_r1, ceiling_r2};
  fx.grant_authority(ceiling_r1, ceiling_r2, 0, 10000000);

  std::uint64_t tick = 1000;
  std::size_t highest_stage = 0;
  for (int step_index = 0; step_index < 60; ++step_index) {
    tick += 1u + rng.below(40u);
    const std::uint64_t rate = rng.below(2600u);
    const bool cleared = rng.chance(3u, 4u);
    RestorationVector applied{};
    (void)applied.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, rng.below(ceiling_r1 + 200u));
    (void)applied.set_level(kR2, RestorationUnit::BYTES_PER_SECOND, rng.below(ceiling_r2 + 200u));

    EvidenceSnapshot snapshot = fx.snapshot(tick, rate, cleared, &applied);
    // Corrupt the binding some of the time so stale rejection is exercised.
    const std::uint64_t mode = rng.below(10u);
    bool stale = false;
    if (mode == 0u) {
      snapshot.plan_generation = PlanGeneration(999);
      stale = true;
    } else if (mode == 1u) {
      snapshot.epoch = CoordinatorEpoch(77);
      stale = true;
    } else if (mode == 2u) {
      for (Evidence& e : snapshot.entries) {
        e.observed_at_tick = tick > 5000u ? tick - 5000u : 0u;
      }
      stale = true;
    }

    const PlanGeneration generation = fx.engine.plan(fx.handle.id)->generation;
    const RecoveryState before = fx.engine.plan(fx.handle.id)->state;
    const std::size_t stage_before = fx.engine.plan(fx.handle.id)->stage_index;
    const StageDecision decision = fx.engine.advance(fx.handle.id, generation, snapshot);
    if (stale) {
      CHK(!decision.advanced);
    }
    if (decision.advanced) {
      CHK(is_valid(before));
      CHK(fx.engine.plan(fx.handle.id)->stage_index >= stage_before);
    }
    // A pure evaluation must agree with the committed one and change nothing.
    const RecoveryPlan snapshot_before = *fx.engine.plan(fx.handle.id);
    const StageDecision preview = fx.engine.evaluate(fx.handle.id, generation,
                                                     fx.snapshot(tick + 1000, 5000, true, nullptr));
    (void)preview;
    const RecoveryPlan snapshot_after = *fx.engine.plan(fx.handle.id);
    CHK(snapshot_before.stage_index == snapshot_after.stage_index);
    CHK(snapshot_before.attempt == snapshot_after.attempt);
    CHK(snapshot_before.updated_at_tick == snapshot_after.updated_at_tick);
    CHK(snapshot_before.completions.size() == snapshot_after.completions.size());

    check_invariants(fx, ceiling, highest_stage);

    RecoveryExplanation explanation{};
    CHK(fx.engine.explain(fx.handle.id, explanation));
    CHK(!fx.engine.render_explanation(fx.handle.id).empty());

    if (fx.engine.plan(fx.handle.id)->state == RecoveryState::ROLLING_BACK) {
      RollbackRequest request{};
      request.plan = fx.handle.id;
      request.plan_generation = fx.engine.plan(fx.handle.id)->generation;
      request.epoch = fx.epoch;
      request.observed_current = make_observed(rng.below(ceiling_r1), rng.below(ceiling_r2));
      request.provenance = Provenance::MEASURED;
      request.now_tick = tick + 5;
      request.reason = "property walk recurrence";
      const RollbackOutcome outcome = fx.engine.rollback(request);
      CHK(outcome.ok);
      // Compensation never increases a level.
      for (const RestorationDelta& delta : outcome.compensation.steps) {
        CHK(delta.delta <= 0);
      }
      check_invariants(fx, ceiling, highest_stage);
    }
    if (fx.engine.plan(fx.handle.id)->state == RecoveryState::PAUSED ||
        fx.engine.plan(fx.handle.id)->state == RecoveryState::ROLLED_BACK) {
      (void)fx.engine.resume(fx.handle.id, fx.engine.plan(fx.handle.id)->generation, tick + 50);
    }
  }

  // Durability must round trip after any walk.
  const DurableState state = fx.engine.export_state();
  std::vector<std::uint8_t> bytes;
  std::string err;
  CHK(serialize_state(state, EngineLimits{}, bytes, err));
  DurableState parsed{};
  CHK(deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));

  // Idempotent completion: replaying the last committed completion twice must
  // not change the completion count.
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  if (plan != nullptr && !plan->completions.empty() &&
      plan->completions.back().state == CompletionState::COMMITTED) {
    const std::size_t count = plan->completions.size();
    const std::uint32_t committed = plan->completion_count;
    const std::uint64_t tick_now = plan->updated_at_tick;
    const RestorationVector final_levels = plan->authorized;
    const EvidenceSnapshot replay_snapshot = fx.snapshot(tick_now, 2000, true, &final_levels);
    EvidenceSnapshot bound = replay_snapshot;
    bound.generation = SnapshotGeneration(tick_now);
    bound.plan_generation = plan->generation;
    bound.epoch = fx.engine.epoch();
    for (Evidence& e : bound.entries) {
      e.plan_generation = plan->generation;
      e.epoch = fx.engine.epoch();
      e.observed_at_tick = tick_now;
    }
    (void)fx.engine.complete(fx.handle.id, plan->generation, bound);
    const RecoveryPlan* after = fx.engine.plan(fx.handle.id);
    CHK(after != nullptr && after->completions.size() >= count);
    CHK(after != nullptr && after->completion_count == committed);
  }
}

void test_stale_never_advances() {
  SECTION("stale evidence never advances a stage");
  Fixture fx;
  CHK(fx.build());
  const RestorationVector applied = target_for(0);
  for (std::uint64_t offset = 1; offset <= 64; offset *= 2u) {
    EvidenceSnapshot snapshot = fx.snapshot(1020, rate_for(0), true, &applied);
    for (Evidence& e : snapshot.entries) {
      e.observed_at_tick = 1020u > offset ? 1020u - offset : 0u;
    }
    const StageDecision d = fx.engine.advance(fx.handle.id, fx.handle.generation, snapshot);
    if (offset > fx.cfg.max_evidence_age) {
      CHK(!d.advanced);
      CHK(d.decision == RecoveryDecision::REJECT_EVIDENCE);
    }
  }
}

void test_authority_ceiling_is_never_exceeded() {
  SECTION("restoration never exceeds authority in a randomized sweep");
  Rng rng(0xC0FFEEu);
  for (int round = 0; round < 60; ++round) {
    Fixture fx;
    if (!fx.build()) {
      continue;
    }
    const std::uint64_t ceiling_r1 = 200u + rng.below(600u);
    const std::uint64_t ceiling_r2 = 200u + rng.below(400u);
    fx.grant_authority(ceiling_r1, ceiling_r2, 0, 10000000);
    std::uint64_t tick = 1000;
    for (int step_index = 0; step_index < 12; ++step_index) {
      tick += 20;
      const std::size_t stage = fx.engine.plan(fx.handle.id)->stage_index;
      const RestorationVector applied = target_for(stage);
      const StageDecision d = step(fx, tick, rate_for(stage), true, &applied);
      // Only a restoration rung is bounded by authority; OBSERVE mirrors the
      // constrained state, which the plan does not control.
      const bool restoring = d.stage_kind_before != StageKind::OBSERVE;
      for (const RestorationAmount& amount : d.effective.entries()) {
        const std::uint64_t limit = amount.resource == kR1 ? ceiling_r1 : ceiling_r2;
        const std::uint64_t constrained = amount.resource == kR1 ? kConstrainedR1 : kConstrainedR2;
        CHK(amount.effective <= (restoring ? limit : max_u64(limit, constrained)));
      }
      const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
      for (const RestorationAmount& amount : plan->authorized.entries()) {
        const std::uint64_t limit = amount.resource == kR1 ? ceiling_r1 : ceiling_r2;
        const std::uint64_t constrained = amount.resource == kR1 ? kConstrainedR1 : kConstrainedR2;
        CHK(amount.effective <= max_u64(limit, constrained));
      }
      if (plan->state == RecoveryState::COMPLETED) {
        break;
      }
    }
  }
}

}  // namespace

int main() {
  crtest::init();
  SECTION("seeded randomized recovery walks");
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    random_walk(seed * 2654435761ull + 1ull);
  }
  test_stale_never_advances();
  test_authority_ceiling_is_never_exceeded();
  return summary("cr_property");
}
