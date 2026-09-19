// Congestion Recovery example - recurrence, compensating rollback, retry.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <cstdio>
#include <string>

#include "congestion_recovery/engine.hpp"

using namespace congestion_recovery;

namespace {

constexpr ResourceId kLink{1001};

EvidenceRequirement require_cleared() {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::CONGESTION_CLEARED;
  r.min_provenance = Provenance::MEASURED;
  r.min_confidence = 0.5;
  r.min_samples = 1;
  r.require_affirmative = true;
  return r;
}

EvidenceRequirement require_rate(std::uint64_t min_value) {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::SERVICE_RATE;
  r.min_provenance = Provenance::MEASURED;
  r.min_confidence = 0.5;
  r.min_samples = 3;
  r.min_duration_ticks = 20;
  r.require_stable = true;
  r.stability_window_ticks = 20;
  r.has_min_value = true;
  r.min_value = min_value;
  r.primary = true;
  return r;
}

StageSpec stage(StageKind kind, const char* name, std::uint64_t link, std::uint64_t rate, bool authority) {
  StageSpec s{};
  s.kind = kind;
  s.name = name;
  (void)s.target.set_level(kLink, RestorationUnit::BYTES_PER_SECOND, link);
  s.dwell_ticks = 10;
  s.max_attempts = 3;
  s.requires_authority = authority;
  s.rollback_on_failure = true;
  s.requirements.push_back(require_cleared());
  s.requirements.push_back(require_rate(rate));
  return s;
}

RestorationVector levels(std::uint64_t link) {
  RestorationVector v{};
  (void)v.set_level(kLink, RestorationUnit::BYTES_PER_SECOND, link);
  return v;
}

EvidenceSnapshot make_snapshot(const RecoveryEngine& engine, const PlanHandle& handle, std::uint64_t tick,
                               std::uint64_t rate, bool cleared, std::uint64_t applied_link) {
  EvidenceSnapshot snapshot{};
  snapshot.id = EvidenceSnapshotId(tick);
  snapshot.generation = SnapshotGeneration(tick);
  const RecoveryPlan* plan = engine.plan(handle.id);
  snapshot.plan_generation = plan->generation;
  snapshot.policy_generation = plan->policy_generation;
  snapshot.epoch = engine.epoch();
  snapshot.assembled_at_tick = tick;

  Evidence base{};
  base.id = EvidenceId(1);
  base.generation = EvidenceGeneration(1);
  base.provenance = Provenance::MEASURED;
  base.confidence = 0.9;
  base.sample_count = 4;
  base.duration_ticks = 40;
  base.stable = true;
  base.observed_at_tick = tick - 5;
  base.plan_generation = snapshot.plan_generation;
  base.policy_generation = snapshot.policy_generation;
  base.epoch = snapshot.epoch;

  Evidence cleared_evidence = base;
  cleared_evidence.kind = EvidenceKind::CONGESTION_CLEARED;
  cleared_evidence.value = cleared ? 1u : 0u;
  cleared_evidence.affirmative = cleared;
  snapshot.entries.push_back(cleared_evidence);

  Evidence rate_evidence = base;
  rate_evidence.id = EvidenceId(2);
  rate_evidence.generation = EvidenceGeneration(2);
  rate_evidence.kind = EvidenceKind::SERVICE_RATE;
  rate_evidence.value = rate;
  rate_evidence.unit = RestorationUnit::BYTES_PER_SECOND;
  snapshot.entries.push_back(rate_evidence);

  Evidence applied = base;
  applied.id = EvidenceId(3);
  applied.generation = EvidenceGeneration(3);
  applied.kind = EvidenceKind::APPLIED_RESTORATION;
  applied.resource = kLink;
  applied.value = applied_link;
  applied.unit = RestorationUnit::BYTES_PER_SECOND;
  snapshot.entries.push_back(applied);
  return snapshot;
}

}  // namespace

int main() {
  RecoveryEngine engine{EngineLimits{}};
  engine.begin_epoch(CoordinatorEpoch(1));

  RecoveryPolicy policy{};
  policy.generation = PolicyGeneration(1);
  policy.name = "example-recurrence";
  policy.stages.push_back(stage(StageKind::OBSERVE, "observe", 200, 600, false));
  policy.stages.push_back(stage(StageKind::PROBE, "probe", 400, 900, true));
  policy.stages.push_back(stage(StageKind::PARTIAL_RESTORE, "partial", 700, 1350, true));
  policy.stages.push_back(stage(StageKind::HOLD, "hold", 700, 1350, true));
  policy.stages.push_back(stage(StageKind::EXPAND, "expand", 1000, 1800, true));
  policy.stages.push_back(stage(StageKind::COMPLETE, "complete", 1000, 1800, true));
  policy.hysteresis.advance_margin = 100;
  policy.hysteresis.rollback_margin = 50;
  policy.hysteresis.cooldown_ticks = 5;
  policy.hysteresis.max_transitions_per_window = 12;
  policy.hysteresis.window_ticks = 100000;
  policy.max_evidence_age_ticks = 1000;
  policy.max_plan_lifetime_ticks = 100000;
  policy.revalidation_boundary_ticks = 100000;
  policy.on_recurrence = RecurrenceAction::ROLLBACK;

  const PolicyId policy_id = engine.define_policy(policy);
  const RecoveryPolicy* stored = engine.policy(policy_id);

  CreatePlanRequest request{};
  request.policy_id = policy_id;
  request.policy_generation = stored->generation;
  request.congestion = CongestionRefId(11);
  request.intervention = InterventionId(22);
  request.epoch = engine.epoch();
  request.now_tick = 1000;
  request.resources.push_back(ResourceBinding{kLink, ResourceGeneration(1)});
  request.baseline = levels(1000);
  request.baseline_known = true;
  request.constrained = levels(200);
  request.rollback_target = levels(200);
  const PlanHandle handle = engine.create_plan(request);
  if (!handle.is_valid()) {
    std::printf("plan rejected\n");
    return 1;
  }

  RecoveryAuthority grant{};
  grant.id = AuthorityId(1);
  grant.generation = AuthorityGeneration(1);
  grant.epoch = engine.epoch();
  grant.resource = kLink;
  grant.unit = RestorationUnit::BYTES_PER_SECOND;
  grant.ceiling = 1000;
  grant.expires_at_tick = 10000000;
  grant.issuer = PublisherId(1);
  (void)engine.grant_authority(grant);

  StageDecision decision = engine.advance(handle.id, handle.generation,
                                          make_snapshot(engine, handle, 1020, 1000, true, 200));
  std::printf("advance  observe -> %s\n", std::string(to_string(decision.decision)).c_str());
  decision = engine.advance(handle.id, handle.generation,
                            make_snapshot(engine, handle, 1050, 1100, true, 400));
  std::printf("advance  probe   -> %s\n", std::string(to_string(decision.decision)).c_str());

  // Congestion returns during PARTIAL_RESTORE.
  decision = engine.advance(handle.id, handle.generation, make_snapshot(engine, handle, 1080, 100, false, 700));
  std::printf("recurrence       -> decision=%s reason=%s detail=%s\n",
              std::string(to_string(decision.decision)).c_str(),
              std::string(to_string(decision.reason)).c_str(), decision.reason_text.c_str());

  RollbackRequest rollback{};
  rollback.plan = handle.id;
  rollback.plan_generation = engine.plan(handle.id)->generation;
  rollback.epoch = engine.epoch();
  rollback.observed_current = levels(700);
  rollback.observed_current.find_mut(kLink)->observed = 700;
  rollback.observed_current.find_mut(kLink)->observed_known = true;
  rollback.provenance = Provenance::MEASURED;
  rollback.now_tick = 1090;
  rollback.trigger = RejectReason::RECURRENCE_DETECTED;
  rollback.reason = "service rate collapsed during partial restoration";
  const RollbackOutcome outcome = engine.rollback(rollback);
  std::printf("rollback ok=%d kind_complete=%d state=%s\n", outcome.ok ? 1 : 0,
              outcome.compensation.complete ? 1 : 0,
              std::string(to_string(outcome.state_after)).c_str());
  for (const RestorationDelta& step_delta : outcome.compensation.steps) {
    std::printf("  compensating resource=%llu delta=%lld (from observed level)\n",
                static_cast<unsigned long long>(step_delta.resource.value()),
                static_cast<long long>(step_delta.delta));
  }
  for (const ResourceId unobserved : outcome.compensation.unobserved) {
    std::printf("  unobserved resource=%llu requires observation before compensation\n",
                static_cast<unsigned long long>(unobserved.value()));
  }

  const StageDecision resumed = engine.resume(handle.id, engine.plan(handle.id)->generation, 1100);
  std::printf("resume           -> decision=%s state=%s\n",
              std::string(to_string(resumed.decision)).c_str(),
              std::string(to_string(resumed.state_after)).c_str());
  std::printf("\n%s", engine.render_explanation(handle.id).c_str());
  return outcome.ok ? 0 : 1;
}
