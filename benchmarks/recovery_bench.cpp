// Congestion Recovery benchmark - SYNTHETIC recovery-plan evaluation throughput.
// Copyright 2026 Summon Software Labs.
//
// This measures completed recovery-plan work in memory on a synthetic
// population. It is not a physical recovery-time claim: no link, switch, NIC,
// fabric or network is involved, and no packet is ever moved.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

double seconds_since(const std::chrono::steady_clock::time_point& start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double>(elapsed).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t plan_count = 2000;
  if (argc > 1) {
    plan_count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (plan_count == 0u || plan_count > 20000u) {
    std::printf("plan count must be between 1 and 20000\n");
    return 2;
  }

  RecoveryEngine engine{EngineLimits{}};
  engine.begin_epoch(CoordinatorEpoch(1));

  RecoveryPolicy policy{};
  policy.generation = PolicyGeneration(1);
  policy.name = "bench-ladder";
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
  policy.max_plan_lifetime_ticks = 1000000;
  policy.revalidation_boundary_ticks = 1000000;
  const PolicyId policy_id = engine.define_policy(policy);
  const RecoveryPolicy* stored = engine.policy(policy_id);

  std::vector<PlanHandle> handles;
  handles.reserve(plan_count);
  const auto create_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < plan_count; ++i) {
    CreatePlanRequest request{};
    request.policy_id = policy_id;
    request.policy_generation = stored->generation;
    request.congestion = CongestionRefId(i + 1u);
    request.intervention = InterventionId(i + 1u);
    request.epoch = engine.epoch();
    request.now_tick = 1000;
    request.resources.push_back(ResourceBinding{kLink, ResourceGeneration(1)});
    request.resources.push_back(ResourceBinding{kPath, ResourceGeneration(1)});
    request.baseline = levels(1000, 800);
    request.baseline_known = true;
    request.constrained = levels(200, 400);
    request.rollback_target = levels(200, 400);
    const PlanHandle handle = engine.create_plan(request);
    if (handle.is_valid()) {
      handles.push_back(handle);
    }
  }
  const double create_seconds = seconds_since(create_start);

  std::uint64_t authority_id = 1;
  for (const ResourceBinding& binding : std::vector<ResourceBinding>{
           ResourceBinding{kLink, ResourceGeneration(1)}, ResourceBinding{kPath, ResourceGeneration(1)}}) {
    RecoveryAuthority grant{};
    grant.id = AuthorityId(authority_id);
    grant.generation = AuthorityGeneration(authority_id);
    grant.epoch = engine.epoch();
    grant.resource = binding.resource;
    grant.unit = RestorationUnit::BYTES_PER_SECOND;
    grant.ceiling = binding.resource == kLink ? 1000 : 800;
    grant.expires_at_tick = 10000000;
    grant.issuer = PublisherId(1);
    (void)engine.grant_authority(grant);
    ++authority_id;
  }

  const std::uint64_t targets[6][2] = {{200, 400}, {400, 500}, {700, 650}, {700, 650}, {1000, 800}, {1000, 800}};
  const std::uint64_t rates[6] = {900, 1100, 1500, 1500, 2000, 2000};

  std::uint64_t evaluations = 0;
  std::uint64_t completions = 0;
  const auto transition_start = std::chrono::steady_clock::now();
  for (const PlanHandle& handle : handles) {
    for (std::size_t index = 0; index < 6; ++index) {
      const std::uint64_t tick = 1000 + (index + 1) * 20;
      const RecoveryPlan* current = engine.plan(handle.id);
      EvidenceSnapshot snapshot{};
      snapshot.id = EvidenceSnapshotId(tick);
      snapshot.generation = SnapshotGeneration(tick);
      snapshot.plan_generation = current->generation;
      snapshot.policy_generation = current->policy_generation;
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

      Evidence cleared = base;
      cleared.kind = EvidenceKind::CONGESTION_CLEARED;
      cleared.value = 1;
      cleared.affirmative = true;
      snapshot.entries.push_back(cleared);

      Evidence rate = base;
      rate.id = EvidenceId(2);
      rate.generation = EvidenceGeneration(2);
      rate.kind = EvidenceKind::SERVICE_RATE;
      rate.value = rates[index];
      rate.unit = RestorationUnit::BYTES_PER_SECOND;
      snapshot.entries.push_back(rate);

      Evidence applied_link = base;
      applied_link.id = EvidenceId(3);
      applied_link.generation = EvidenceGeneration(3);
      applied_link.kind = EvidenceKind::APPLIED_RESTORATION;
      applied_link.resource = kLink;
      applied_link.value = targets[index][0];
      applied_link.unit = RestorationUnit::BYTES_PER_SECOND;
      snapshot.entries.push_back(applied_link);

      Evidence applied_path = applied_link;
      applied_path.id = EvidenceId(4);
      applied_path.generation = EvidenceGeneration(4);
      applied_path.resource = kPath;
      applied_path.value = targets[index][1];
      snapshot.entries.push_back(applied_path);

      // The pure evaluation is timed separately from the committed transition.
      const StageDecision preview = engine.evaluate(handle.id, handle.generation, snapshot);
      ++evaluations;
      if (preview.reason != RejectReason::NONE) {
        break;
      }
      const StageDecision decision = engine.advance(handle.id, handle.generation, snapshot);
      if (decision.completed) {
        ++completions;
        break;
      }
    }
  }
  const double transition_seconds = seconds_since(transition_start);

  std::printf("SYNTHETIC congestion-recovery plan evaluation benchmark\n");
  std::printf("population=SYNTHETIC plans=%zu resources=2 stages=6\n", handles.size());
  std::printf("policies=1 policy_generation=%llu\n",
              static_cast<unsigned long long>(stored->generation.value()));
  std::printf("plan_create_ops=%zu plan_create_seconds=%.6f plan_create_per_second=%.0f\n", handles.size(),
              create_seconds, create_seconds > 0.0 ? static_cast<double>(handles.size()) / create_seconds : 0.0);
  std::printf("evaluate_ops=%llu evaluate_seconds=%.6f evaluate_per_second=%.0f\n",
              static_cast<unsigned long long>(evaluations), transition_seconds,
              transition_seconds > 0.0 ? static_cast<double>(evaluations) / transition_seconds : 0.0);
  std::printf("completed_plans=%llu engine_advances=%llu engine_evaluations=%llu\n",
              static_cast<unsigned long long>(completions),
              static_cast<unsigned long long>(engine.advances()),
              static_cast<unsigned long long>(engine.evaluations()));
  std::printf("NOTE synthetic in-memory plan transitions only; no physical recovery-time claim\n");
  return completions == handles.size() ? 0 : 1;
}
