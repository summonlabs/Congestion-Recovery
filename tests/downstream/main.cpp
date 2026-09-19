// Congestion Recovery downstream consumer.
// Copyright 2026 Summon Software Labs.
//
// This target links only against the exported package target. It proves the
// public headers, the static library and the CMake package closure are
// sufficient to build and run a real recovery plan from outside the tree.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "congestion_recovery/engine.hpp"

using namespace congestion_recovery;

namespace {

constexpr ResourceId kResource{1};

EvidenceRequirement require_cleared() {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::CONGESTION_CLEARED;
  r.min_provenance = Provenance::MEASURED;
  r.min_samples = 1;
  r.require_affirmative = true;
  return r;
}

EvidenceRequirement require_rate(std::uint64_t min_value) {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::SERVICE_RATE;
  r.min_provenance = Provenance::MEASURED;
  r.min_samples = 2;
  r.min_duration_ticks = 10;
  r.has_min_value = true;
  r.min_value = min_value;
  r.primary = true;
  return r;
}

StageSpec stage(StageKind kind, const char* name, std::uint64_t level, std::uint64_t rate, bool authority) {
  StageSpec s{};
  s.kind = kind;
  s.name = name;
  (void)s.target.set_level(kResource, RestorationUnit::BYTES_PER_SECOND, level);
  s.dwell_ticks = 5;
  s.max_attempts = 2;
  s.requires_authority = authority;
  s.requirements.push_back(require_cleared());
  s.requirements.push_back(require_rate(rate));
  return s;
}

RestorationVector levels(std::uint64_t value) {
  RestorationVector v{};
  (void)v.set_level(kResource, RestorationUnit::BYTES_PER_SECOND, value);
  return v;
}

void fill(EvidenceSnapshot& snapshot, const RecoveryPlan& plan, CoordinatorEpoch epoch, std::uint64_t tick,
          std::uint64_t rate, std::uint64_t applied) {
  snapshot.id = EvidenceSnapshotId(tick);
  snapshot.generation = SnapshotGeneration(tick);
  snapshot.plan_generation = plan.generation;
  snapshot.policy_generation = plan.policy_generation;
  snapshot.epoch = epoch;
  snapshot.assembled_at_tick = tick;

  Evidence base{};
  base.id = EvidenceId(1);
  base.generation = EvidenceGeneration(1);
  base.resource = kResource;
  base.provenance = Provenance::MEASURED;
  base.confidence = 0.9;
  base.sample_count = 3;
  base.duration_ticks = 20;
  base.stable = true;
  base.observed_at_tick = tick - 2;
  base.plan_generation = plan.generation;
  base.policy_generation = plan.policy_generation;
  base.epoch = epoch;

  Evidence cleared = base;
  cleared.kind = EvidenceKind::CONGESTION_CLEARED;
  cleared.value = 1;
  cleared.affirmative = true;
  snapshot.entries.push_back(cleared);

  Evidence rate_evidence = base;
  rate_evidence.id = EvidenceId(2);
  rate_evidence.generation = EvidenceGeneration(2);
  rate_evidence.kind = EvidenceKind::SERVICE_RATE;
  rate_evidence.value = rate;
  rate_evidence.unit = RestorationUnit::BYTES_PER_SECOND;
  snapshot.entries.push_back(rate_evidence);

  Evidence applied_evidence = base;
  applied_evidence.id = EvidenceId(3);
  applied_evidence.generation = EvidenceGeneration(3);
  applied_evidence.kind = EvidenceKind::APPLIED_RESTORATION;
  applied_evidence.value = applied;
  applied_evidence.unit = RestorationUnit::BYTES_PER_SECOND;
  snapshot.entries.push_back(applied_evidence);
}

}  // namespace

int main() {
  RecoveryEngine engine{EngineLimits{}};
  engine.begin_epoch(CoordinatorEpoch(1));

  RecoveryPolicy policy{};
  policy.generation = PolicyGeneration(1);
  policy.name = "downstream";
  policy.stages.push_back(stage(StageKind::OBSERVE, "observe", 100, 500, false));
  policy.stages.push_back(stage(StageKind::PROBE, "probe", 200, 700, true));
  policy.stages.push_back(stage(StageKind::PARTIAL_RESTORE, "partial", 300, 900, true));
  policy.stages.push_back(stage(StageKind::HOLD, "hold", 300, 900, true));
  policy.stages.push_back(stage(StageKind::EXPAND, "expand", 400, 1100, true));
  policy.stages.push_back(stage(StageKind::COMPLETE, "complete", 400, 1100, true));
  policy.hysteresis.advance_margin = 50;
  policy.hysteresis.rollback_margin = 25;
  policy.hysteresis.max_transitions_per_window = 12;
  policy.hysteresis.window_ticks = 100000;
  policy.max_evidence_age_ticks = 1000;
  policy.max_plan_lifetime_ticks = 100000;
  policy.revalidation_boundary_ticks = 100000;

  const PolicyId policy_id = engine.define_policy(policy);
  if (!policy_id.is_valid()) {
    std::printf("DOWNSTREAM_FAIL policy rejected\n");
    return 1;
  }
  const RecoveryPolicy* stored = engine.policy(policy_id);

  CreatePlanRequest request{};
  request.policy_id = policy_id;
  request.policy_generation = stored->generation;
  request.congestion = CongestionRefId(1);
  request.intervention = InterventionId(2);
  request.epoch = engine.epoch();
  request.now_tick = 100;
  request.resources.push_back(ResourceBinding{kResource, ResourceGeneration(1)});
  request.baseline = levels(400);
  request.baseline_known = true;
  request.constrained = levels(100);
  request.rollback_target = levels(100);

  const PlanHandle handle = engine.create_plan(request);
  if (!handle.is_valid()) {
    std::printf("DOWNSTREAM_FAIL plan rejected\n");
    return 1;
  }
  std::printf("DOWNSTREAM_PLAN id=%llu generation=%llu\n",
              static_cast<unsigned long long>(handle.id.value()),
              static_cast<unsigned long long>(handle.generation.value()));

  RecoveryAuthority grant{};
  grant.id = AuthorityId(1);
  grant.generation = AuthorityGeneration(1);
  grant.epoch = engine.epoch();
  grant.resource = kResource;
  grant.unit = RestorationUnit::BYTES_PER_SECOND;
  grant.ceiling = 400;
  grant.expires_at_tick = 1000000;
  grant.issuer = PublisherId(1);
  if (!engine.grant_authority(grant)) {
    std::printf("DOWNSTREAM_FAIL authority rejected\n");
    return 1;
  }

  const std::uint64_t targets[6] = {100, 200, 300, 300, 400, 400};
  const std::uint64_t rates[6] = {600, 800, 1000, 1000, 1200, 1200};
  std::string state = "UNKNOWN";
  for (std::size_t index = 0; index < 6; ++index) {
    const std::uint64_t tick = 100 + (index + 1) * 10;
    const RecoveryPlan* plan = engine.plan(handle.id);
    EvidenceSnapshot snapshot{};
    fill(snapshot, *plan, engine.epoch(), tick, rates[index], targets[index]);
    const StageDecision decision = engine.advance(handle.id, handle.generation, snapshot);
    std::printf("DOWNSTREAM_STAGE index=%zu decision=%s reason=%s\n", index,
                std::string(to_string(decision.decision)).c_str(),
                std::string(to_string(decision.reason)).c_str());
    const RecoveryPlan* after = engine.plan(handle.id);
    state = std::string(to_string(after->state));
    if (after->state == RecoveryState::COMPLETED) {
      break;
    }
  }

  const std::string rendered = engine.render_explanation(handle.id);
  std::printf("DOWNSTREAM_STATE %s\n", state.c_str());
  std::printf("DOWNSTREAM_EXPLANATION_LINES %zu\n", static_cast<std::size_t>(std::count(rendered.begin(), rendered.end(), '\n')));
  std::printf("DOWNSTREAM_OK\n");
  return state == "COMPLETED" ? 0 : 1;
}
