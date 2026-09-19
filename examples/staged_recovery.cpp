// Congestion Recovery example - a full staged restoration with explanations.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <cstdio>
#include <string>

#include "congestion_recovery/engine.hpp"

using namespace congestion_recovery;

namespace {

constexpr ResourceId kLink{1001};
constexpr ResourceId kPath{1002};

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

StageSpec stage(StageKind kind, const char* name, std::uint64_t link, std::uint64_t path, std::uint64_t rate,
                bool authority) {
  StageSpec s{};
  s.kind = kind;
  s.name = name;
  (void)s.target.set_level(kLink, RestorationUnit::BYTES_PER_SECOND, link);
  (void)s.target.set_level(kPath, RestorationUnit::BYTES_PER_SECOND, path);
  s.dwell_ticks = 10;
  s.max_attempts = 3;
  s.requires_authority = authority;
  s.rollback_on_failure = true;
  s.requirements.push_back(require_cleared());
  s.requirements.push_back(require_rate(rate));
  return s;
}

RestorationVector levels(std::uint64_t link, std::uint64_t path) {
  RestorationVector v{};
  (void)v.set_level(kLink, RestorationUnit::BYTES_PER_SECOND, link);
  (void)v.set_level(kPath, RestorationUnit::BYTES_PER_SECOND, path);
  return v;
}

}  // namespace

int main() {
  RecoveryEngine engine{EngineLimits{}};
  engine.begin_epoch(CoordinatorEpoch(1));

  RecoveryPolicy policy{};
  policy.generation = PolicyGeneration(1);
  policy.name = "example-ladder";
  policy.stages.push_back(stage(StageKind::OBSERVE, "observe", 200, 400, 600, false));
  policy.stages.push_back(stage(StageKind::PROBE, "probe", 400, 500, 900, true));
  policy.stages.push_back(stage(StageKind::PARTIAL_RESTORE, "partial", 700, 650, 1350, true));
  policy.stages.push_back(stage(StageKind::HOLD, "hold", 700, 650, 1350, true));
  policy.stages.push_back(stage(StageKind::EXPAND, "expand", 1000, 800, 1800, true));
  policy.stages.push_back(stage(StageKind::COMPLETE, "complete", 1000, 800, 1800, true));
  policy.hysteresis.advance_margin = 100;
  policy.hysteresis.rollback_margin = 50;
  policy.hysteresis.cooldown_ticks = 5;
  policy.hysteresis.max_transitions_per_window = 12;
  policy.hysteresis.window_ticks = 100000;
  policy.max_evidence_age_ticks = 1000;
  policy.max_plan_lifetime_ticks = 100000;
  policy.revalidation_boundary_ticks = 100000;

  const PolicyId policy_id = engine.define_policy(policy);
  if (!policy_id.is_valid()) {
    std::printf("policy rejected\n");
    return 1;
  }
  const RecoveryPolicy* stored = engine.policy(policy_id);

  CreatePlanRequest request{};
  request.policy_id = policy_id;
  request.policy_generation = stored->generation;
  request.congestion = CongestionRefId(11);
  request.intervention = InterventionId(22);
  request.epoch = engine.epoch();
  request.now_tick = 1000;
  request.resources.push_back(ResourceBinding{kLink, ResourceGeneration(1)});
  request.resources.push_back(ResourceBinding{kPath, ResourceGeneration(1)});
  request.baseline = levels(1000, 800);
  request.baseline_known = true;
  request.constrained = levels(200, 400);
  request.rollback_target = levels(200, 400);

  const PlanHandle handle = engine.create_plan(request);
  if (!handle.is_valid()) {
    std::printf("plan rejected\n");
    return 1;
  }

  std::uint64_t authority_id = 1;
  for (const ResourceBinding& binding : request.resources) {
    RecoveryAuthority grant{};
    grant.id = AuthorityId(authority_id);
    grant.generation = AuthorityGeneration(authority_id);
    grant.epoch = engine.epoch();
    grant.resource = binding.resource;
    grant.unit = RestorationUnit::BYTES_PER_SECOND;
    grant.ceiling = binding.resource == kLink ? 1000 : 800;
    grant.expires_at_tick = 10000000;
    grant.issuer = PublisherId(1);
    if (!engine.grant_authority(grant)) {
      std::printf("authority rejected for resource %llu\n",
                  static_cast<unsigned long long>(binding.resource.value()));
      return 1;
    }
    ++authority_id;
  }

  const std::uint64_t targets[6][2] = {{200, 400}, {400, 500}, {700, 650}, {700, 650}, {1000, 800}, {1000, 800}};
  const std::uint64_t rates[6] = {900, 1100, 1500, 1500, 2000, 2000};
  std::string last_state;

  for (std::size_t index = 0; index < 6; ++index) {
    const std::uint64_t tick = 1000 + (index + 1) * 20;
    EvidenceSnapshot snapshot{};
    snapshot.id = EvidenceSnapshotId(tick);
    snapshot.generation = SnapshotGeneration(tick);
    const RecoveryPlan* current = engine.plan(handle.id);
    snapshot.plan_generation = current->generation;
    snapshot.policy_generation = current->policy_generation;
    snapshot.epoch = engine.epoch();
    snapshot.assembled_at_tick = tick;

    Evidence cleared{};
    cleared.id = EvidenceId(1);
    cleared.generation = EvidenceGeneration(1);
    cleared.kind = EvidenceKind::CONGESTION_CLEARED;
    cleared.value = 1;
    cleared.affirmative = true;
    cleared.confidence = 0.95;
    cleared.sample_count = 4;
    cleared.duration_ticks = 40;
    cleared.provenance = Provenance::MEASURED;
    cleared.stable = true;
    cleared.observed_at_tick = tick - 5;
    cleared.plan_generation = snapshot.plan_generation;
    cleared.policy_generation = snapshot.policy_generation;
    cleared.epoch = snapshot.epoch;
    snapshot.entries.push_back(cleared);

    Evidence rate = cleared;
    rate.id = EvidenceId(2);
    rate.generation = EvidenceGeneration(2);
    rate.kind = EvidenceKind::SERVICE_RATE;
    rate.value = rates[index];
    rate.unit = RestorationUnit::BYTES_PER_SECOND;
    snapshot.entries.push_back(rate);

    for (const ResourceBinding& binding : request.resources) {
      Evidence applied = cleared;
      applied.id = EvidenceId(3);
      applied.generation = EvidenceGeneration(3);
      applied.kind = EvidenceKind::APPLIED_RESTORATION;
      applied.resource = binding.resource;
      applied.value = binding.resource == kLink ? targets[index][0] : targets[index][1];
      applied.unit = RestorationUnit::BYTES_PER_SECOND;
      snapshot.entries.push_back(applied);
    }

    const StageDecision decision = engine.advance(handle.id, handle.generation, snapshot);
    std::printf("tick=%llu stage=%s decision=%s reason=%s\n",
                static_cast<unsigned long long>(tick),
                std::string(to_string(decision.stage_kind_before)).c_str(),
                std::string(to_string(decision.decision)).c_str(),
                std::string(to_string(decision.reason)).c_str());
    if (decision.reason != RejectReason::NONE) {
      std::printf("  detail: %s\n", decision.reason_text.c_str());
    }
    const RecoveryPlan* after = engine.plan(handle.id);
    last_state = std::string(to_string(after->state));
    if (after->state == RecoveryState::COMPLETED) {
      break;
    }
  }

  std::printf("\nfinal state: %s\n\n", last_state.c_str());
  std::printf("%s", engine.render_explanation(handle.id).c_str());
  return last_state == "COMPLETED" ? 0 : 1;
}
