// Congestion Recovery - shared test fixtures.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/engine.hpp"
#include "test_util.hpp"

namespace crtest {

using namespace congestion_recovery;

inline constexpr ResourceId kR1{1001};
inline constexpr ResourceId kR2{1002};

inline constexpr std::uint64_t kBaselineR1 = 1000;
inline constexpr std::uint64_t kBaselineR2 = 800;
inline constexpr std::uint64_t kConstrainedR1 = 200;
inline constexpr std::uint64_t kConstrainedR2 = 400;

struct LadderConfig {
  std::uint64_t dwell{10};
  std::uint32_t max_attempts{3};
  std::uint32_t max_rollbacks{3};
  std::uint32_t max_total_attempts{32};
  std::uint64_t advance_margin{100};
  std::uint64_t rollback_margin{50};
  std::uint64_t cooldown{5};
  // A complete ladder is five transitions; the bound must leave room for at
  // least one rollback and retry before it pauses a healthy recovery.
  std::uint32_t max_transitions{12};
  std::uint64_t window{10000};
  std::uint64_t max_evidence_age{100};
  std::uint64_t revalidation_boundary{10000};
  std::uint64_t max_plan_lifetime{1000000};
  RecurrenceAction on_recurrence{RecurrenceAction::ROLLBACK};
  bool require_completion_evidence{true};
  bool require_stable_completion{true};
  bool baseline_required{true};
  std::uint32_t partial_tolerance_bp{0};
  std::uint64_t probe_target_r1{400};
  std::uint64_t probe_target_r2{500};
  bool complete_rollback_on_failure{true};
};

inline EvidenceRequirement req_cleared() {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::CONGESTION_CLEARED;
  r.min_provenance = Provenance::REPORTED;
  r.min_confidence = 0.5;
  r.min_samples = 1;
  r.min_duration_ticks = 5;
  r.require_affirmative = true;
  return r;
}

inline EvidenceRequirement req_rate(std::uint64_t min_value) {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::SERVICE_RATE;
  r.min_provenance = Provenance::REPORTED;
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

inline StageSpec make_stage(StageKind kind, const char* name, std::uint64_t r1, std::uint64_t r2,
                            std::uint64_t min_rate, bool authority, std::uint64_t dwell,
                            std::uint32_t max_attempts = 3) {
  StageSpec s{};
  s.kind = kind;
  s.name = name;
  (void)s.target.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, r1);
  (void)s.target.set_level(kR2, RestorationUnit::BYTES_PER_SECOND, r2);
  s.dwell_ticks = dwell;
  s.max_attempts = max_attempts;
  s.stability_window_ticks = 20;
  s.requirements.push_back(req_cleared());
  s.requirements.push_back(req_rate(min_rate));
  s.requires_authority = authority;
  s.rollback_on_failure = true;
  return s;
}

inline RecoveryPolicy make_policy(const LadderConfig& cfg = LadderConfig{}) {
  RecoveryPolicy p{};
  p.generation = PolicyGeneration(1);
  p.name = "staged-recovery";
  p.stages.push_back(make_stage(StageKind::OBSERVE, "observe", kConstrainedR1, kConstrainedR2, 600, false,
                                cfg.dwell, cfg.max_attempts));
  p.stages.push_back(make_stage(StageKind::PROBE, "probe", cfg.probe_target_r1, cfg.probe_target_r2, 900, true,
                                cfg.dwell, cfg.max_attempts));
  p.stages.push_back(make_stage(StageKind::PARTIAL_RESTORE, "partial_restore", 700, 650, 1350, true, cfg.dwell,
                                cfg.max_attempts));
  p.stages.push_back(make_stage(StageKind::HOLD, "hold", 700, 650, 1350, true, cfg.dwell, cfg.max_attempts));
  p.stages.push_back(make_stage(StageKind::EXPAND, "expand", kBaselineR1, kBaselineR2, 1800, true, cfg.dwell,
                                cfg.max_attempts));
  p.stages.push_back(make_stage(StageKind::COMPLETE, "complete", kBaselineR1, kBaselineR2, 1800, true,
                                cfg.dwell, cfg.max_attempts));
  p.stages[5].rollback_on_failure = cfg.complete_rollback_on_failure;
  p.hysteresis.advance_margin = cfg.advance_margin;
  p.hysteresis.rollback_margin = cfg.rollback_margin;
  p.hysteresis.cooldown_ticks = cfg.cooldown;
  p.hysteresis.max_transitions_per_window = cfg.max_transitions;
  p.hysteresis.window_ticks = cfg.window;
  p.on_recurrence = cfg.on_recurrence;
  p.max_evidence_age_ticks = cfg.max_evidence_age;
  p.max_plan_lifetime_ticks = cfg.max_plan_lifetime;
  p.revalidation_boundary_ticks = cfg.revalidation_boundary;
  p.max_rollbacks = cfg.max_rollbacks;
  p.max_total_attempts = cfg.max_total_attempts;
  p.partial_application_tolerance_bp = cfg.partial_tolerance_bp;
  p.baseline_required = cfg.baseline_required;
  p.require_completion_evidence = cfg.require_completion_evidence;
  p.require_stable_completion = cfg.require_stable_completion;
  return p;
}

inline RestorationVector make_levels(std::uint64_t r1, std::uint64_t r2) {
  RestorationVector v{};
  (void)v.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, r1);
  (void)v.set_level(kR2, RestorationUnit::BYTES_PER_SECOND, r2);
  return v;
}

// A complete, runnable recovery scenario with two resources.
struct Fixture {
  RecoveryEngine engine{EngineLimits{}};
  LadderConfig cfg{};
  PolicyId policy{};
  PolicyGeneration policy_generation{1};
  PlanHandle handle{};
  CoordinatorEpoch epoch{1};
  std::uint64_t authority_generation{1};

  bool build(const LadderConfig& config = LadderConfig{}) {
    cfg = config;
    const RecoveryPolicy p = make_policy(cfg);
    policy = engine.define_policy(p);
    if (!policy.is_valid()) {
      return false;
    }
    const RecoveryPolicy* stored = engine.policy(policy);
    if (stored == nullptr) {
      return false;
    }
    policy_generation = stored->generation;

    CreatePlanRequest request{};
    request.policy_id = policy;
    request.policy_generation = policy_generation;
    request.congestion = CongestionRefId(77);
    request.intervention = InterventionId(88);
    request.epoch = epoch;
    request.now_tick = 1000;
    request.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
    request.resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
    request.baseline = make_levels(kBaselineR1, kBaselineR2);
    request.baseline_known = true;
    request.constrained = make_levels(kConstrainedR1, kConstrainedR2);
    request.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);
    handle = engine.create_plan(request);
    if (!handle.is_valid()) {
      return false;
    }
    return grant_authority(kBaselineR1, kBaselineR2, 0, 1000000);
  }

  bool grant_authority(std::uint64_t ceiling_r1, std::uint64_t ceiling_r2, std::uint64_t floor,
                       std::uint64_t expires) {
    bool ok = true;
    ok = grant_one(kR1, ceiling_r1, floor, expires) && ok;
    ok = grant_one(kR2, ceiling_r2, floor, expires) && ok;
    return ok;
  }

  bool grant_one(ResourceId resource, std::uint64_t ceiling, std::uint64_t floor, std::uint64_t expires) {
    RecoveryAuthority a{};
    a.id = AuthorityId(authority_generation);
    a.generation = AuthorityGeneration(authority_generation);
    a.epoch = epoch;
    a.resource = resource;
    a.unit = RestorationUnit::BYTES_PER_SECOND;
    a.ceiling = ceiling;
    a.floor = floor;
    a.granted_at_tick = 0;
    a.expires_at_tick = expires;
    a.issuer = PublisherId(5);
    ++authority_generation;
    return engine.grant_authority(a);
  }

  [[nodiscard]] Evidence make_evidence(EvidenceKind kind, std::uint64_t value, std::uint64_t tick,
                                       ResourceId resource, std::uint64_t id) const {
    const RecoveryPlan* current = engine.plan(handle.id);
    Evidence e{};
    e.id = EvidenceId(id);
    e.generation = EvidenceGeneration(id);
    e.kind = kind;
    e.resource = resource;
    e.plan_generation = current != nullptr ? current->generation : PlanGeneration(1);
    e.stage_generation = current != nullptr ? current->stage_generation : StageGeneration(1);
    e.epoch = engine.epoch();
    e.resource_generation = ResourceGeneration(3);
    e.policy_generation = policy_generation;
    e.publisher = PublisherId(9);
    e.worker = WorkerId(2);
    e.boot = WorkerBoot(7);
    e.incarnation = PublisherIncarnation(1);
    e.provenance = Provenance::MEASURED;
    e.confidence = 0.9;
    e.observed_at_tick = tick >= 5 ? tick - 5 : tick;
    e.duration_ticks = 30;
    e.sample_count = 4;
    e.value = value;
    e.unit = RestorationUnit::BYTES_PER_SECOND;
    e.stable = true;
    e.affirmative = value != 0;
    return e;
  }

  [[nodiscard]] EvidenceSnapshot snapshot(std::uint64_t tick, std::uint64_t rate, bool cleared,
                                          const RestorationVector* applied) const {
    EvidenceSnapshot s{};
    s.id = EvidenceSnapshotId(1);
    const RecoveryPlan* current = engine.plan(handle.id);
    s.generation = SnapshotGeneration(tick);
    s.plan_generation = current != nullptr ? current->generation : PlanGeneration(1);
    s.policy_generation = policy_generation;
    s.epoch = engine.epoch();
    s.assembled_at_tick = tick;
    std::uint64_t id = 1;
    s.entries.push_back(make_evidence(EvidenceKind::CONGESTION_CLEARED, cleared ? 1u : 0u, tick, ResourceId{},
                                      id++));
    s.entries.push_back(make_evidence(EvidenceKind::SERVICE_RATE, rate, tick, ResourceId{}, id++));
    if (applied != nullptr) {
      for (const RestorationAmount& a : applied->entries()) {
        s.entries.push_back(
            make_evidence(EvidenceKind::APPLIED_RESTORATION, a.effective, tick, a.resource, id++));
      }
    }
    return s;
  }
};

// An observed applied-level vector: what the caller actually saw applied.
inline RestorationVector make_observed(std::uint64_t r1, std::uint64_t r2) {
  RestorationVector v{};
  RestorationAmount a{};
  a.resource = kR1;
  a.unit = RestorationUnit::BYTES_PER_SECOND;
  a.requested = r1;
  a.authorized = r1;
  a.effective = r1;
  a.observed = r1;
  a.observed_known = true;
  (void)v.put(a);
  a.resource = kR2;
  a.requested = r2;
  a.authorized = r2;
  a.effective = r2;
  a.observed = r2;
  a.observed_known = true;
  (void)v.put(a);
  return v;
}

// Drives one evaluation step with a comfortable tick and a satisfied rate.
inline StageDecision step(Fixture& fx, std::uint64_t tick, std::uint64_t rate, bool cleared,
                          const RestorationVector* applied) {
  const EvidenceSnapshot s = fx.snapshot(tick, rate, cleared, applied);
  return fx.engine.advance(fx.handle.id, fx.handle.generation, s);
}

// Stage index -> the rate the primary requirement needs (plus the advance margin).
inline std::uint64_t rate_for(std::size_t stage_index) {
  switch (stage_index) {
    case 0: return 900;
    case 1: return 1100;
    case 2: return 1500;
    case 3: return 1500;
    case 4: return 2000;
    default: return 2000;
  }
}

inline RestorationVector target_for(std::size_t stage_index) {
  switch (stage_index) {
    case 0: return make_levels(kConstrainedR1, kConstrainedR2);
    case 1: return make_levels(400, 500);
    case 2: return make_levels(700, 650);
    case 3: return make_levels(700, 650);
    default: return make_levels(kBaselineR1, kBaselineR2);
  }
}

}  // namespace crtest
