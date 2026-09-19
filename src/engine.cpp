// Congestion Recovery - deterministic recovery engine.
// Copyright 2026 Summon Software Labs.
//
// Locking discipline (verified by the deadlock/lock-reentrancy audit):
//  * every public method takes mutex_ exactly once, at entry, and never calls
//    another public method;
//  * no caller callback, socket write or file I/O happens while mutex_ is held;
//  * no code path takes a second lock, so there is no lock ordering to reverse;
//  * the engine owns no threads and joins nothing.
#include "congestion_recovery/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace congestion_recovery {

struct RecoveryEngine::PolicySlot {
  RecoveryPolicy policy{};
};

struct RecoveryEngine::PlanSlot {
  RecoveryPlan plan{};
};

namespace {

constexpr double kConfidenceEpsilon = 1.0e-9;
constexpr std::size_t kTransitionWindowRecords = 64;

std::string bound_text(const std::string& text, std::uint32_t max_len) {
  const std::size_t max_size = static_cast<std::size_t>(max_len);
  if (text.size() <= max_size) {
    return text;
  }
  return text.substr(0, max_size);
}

std::string resource_text(ResourceId resource) {
  return std::to_string(resource.value());
}

std::size_t count_recent(const std::vector<std::uint64_t>& ticks, std::uint64_t now,
                         std::uint64_t window) noexcept {
  std::size_t n = 0;
  for (const std::uint64_t t : ticks) {
    if (t <= now && (now - t) <= window) {
      ++n;
    }
  }
  return n;
}

void push_transition(RecoveryPlan& plan, std::uint64_t tick) {
  plan.transition_ticks.push_back(tick);
  while (plan.transition_ticks.size() > kTransitionWindowRecords) {
    plan.transition_ticks.erase(plan.transition_ticks.begin());
  }
}

// Opens (or reuses) the lineage record for the stage currently occupied.
StageRecord& open_stage_record(RecoveryPlan& plan, const StageSpec& stage, std::uint64_t tick) {
  if (!plan.stage_history.empty()) {
    StageRecord& last = plan.stage_history.back();
    if (last.outcome == StageOutcome::PENDING && last.index == plan.stage_index) {
      last.attempt = plan.attempt;
      last.generation = plan.stage_generation;
      return last;
    }
  }
  StageRecord record{};
  record.index = plan.stage_index;
  record.kind = stage.kind;
  record.generation = plan.stage_generation;
  record.attempt = plan.attempt;
  record.entered_tick = tick;
  record.outcome = StageOutcome::PENDING;
  plan.stage_history.push_back(record);
  return plan.stage_history.back();
}

// Invalidates a draft plan in place (used by the pure decision path).
void invalidate_draft(RecoveryPlan& plan, RejectReason reason, const std::string& detail,
                      std::uint32_t max_len) {
  if (!is_live(plan.state)) {
    return;  // terminal plans are never retroactively invalidated
  }
  plan.revalidation_required = true;
  plan.state = RecoveryState::REVALIDATION_REQUIRED;
  plan.last_reason = reason;
  plan.last_reason_text = bound_text(detail, max_len);
  if (plan.revalidation_notes.size() < static_cast<std::size_t>(max_len)) {
    plan.revalidation_notes.push_back(plan.last_reason_text);
  }
}

StageRecord* pending_stage_record(RecoveryPlan& plan) {
  if (plan.stage_history.empty()) {
    return nullptr;
  }
  StageRecord& last = plan.stage_history.back();
  if (last.outcome == StageOutcome::PENDING) {
    return &last;
  }
  return nullptr;
}

}  // namespace

RecoveryEngine::RecoveryEngine(EngineLimits limits) : limits_(limits) {
  if (!limits_.valid()) {
    limits_ = EngineLimits{};
  }
  adjacent_ = AdjacentRequestLog(limits_.max_adjacent_requests);
  epoch_ = CoordinatorEpoch(1);
}

RecoveryEngine::~RecoveryEngine() = default;

const RecoveryEngine::PolicySlot* RecoveryEngine::policy_slot_locked(PolicyId id) const noexcept {
  const auto it = policies_.find(id.value());
  return it == policies_.end() ? nullptr : it->second.get();
}

// A single const overload returns a mutable PlanSlot*: unique_ptr::get() is a
// const member returning T*, so const queries can still operate on a private
// draft copy without exposing mutable state to callers.
RecoveryEngine::PlanSlot* RecoveryEngine::plan_slot_locked(RecoveryPlanId id) const noexcept {
  const auto it = plans_.find(id.value());
  return it == plans_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------
PolicyId RecoveryEngine::define_policy(RecoveryPolicy policy) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (policies_.size() >= static_cast<std::size_t>(limits_.max_policies)) {
    return PolicyId{};
  }
  if (!policy.id.is_valid()) {
    if (next_policy_id_ == kU64Max) {
      return PolicyId{};
    }
    policy.id = PolicyId(next_policy_id_);
  }
  if (!policy.generation.is_valid()) {
    policy.generation = PolicyGeneration(1);
  }
  if (policies_.find(policy.id.value()) != policies_.end()) {
    return PolicyId{};
  }
  std::string err;
  if (!validate_policy(policy, limits_, err)) {
    return PolicyId{};
  }
  const PolicyId id = policy.id;
  auto slot = std::make_unique<PolicySlot>();
  slot->policy = std::move(policy);
  policies_.emplace(id.value(), std::move(slot));
  if (id.value() >= next_policy_id_ && id.value() < kU64Max) {
    next_policy_id_ = id.value() + 1u;
  }
  return id;
}

bool RecoveryEngine::replace_policy(PolicyId id, RecoveryPolicy replacement) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = policies_.find(id.value());
  if (it == policies_.end()) {
    return false;
  }
  replacement.id = id;
  if (replacement.generation <= it->second->policy.generation) {
    return false;  // policy generations never regress
  }
  std::string err;
  if (!validate_policy(replacement, limits_, err)) {
    return false;
  }
  const PolicyGeneration new_generation = replacement.generation;
  it->second->policy = std::move(replacement);
  for (auto& entry : plans_) {
    RecoveryPlan& plan = entry.second->plan;
    if (plan.policy_id == id && !(plan.policy_generation == new_generation)) {
      invalidate_plan_locked(*entry.second, RejectReason::STALE_POLICY, "policy generation advanced");
    }
  }
  return true;
}

const RecoveryPolicy* RecoveryEngine::policy(PolicyId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const PolicySlot* slot = policy_slot_locked(id);
  return slot != nullptr ? &slot->policy : nullptr;
}

std::vector<PolicyId> RecoveryEngine::policy_ids() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<PolicyId> out;
  out.reserve(policies_.size());
  for (const auto& entry : policies_) {
    out.push_back(entry.second->policy.id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------
PlanHandle RecoveryEngine::create_plan(const CreatePlanRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  PlanHandle handle{};
  if (plans_.size() >= static_cast<std::size_t>(limits_.max_plans)) {
    return handle;
  }
  if (!request.epoch.is_valid() || !(request.epoch == epoch_)) {
    return handle;
  }
  if (!request.congestion.is_valid() || !request.intervention.is_valid()) {
    return handle;
  }
  if (request.resources.empty() ||
      request.resources.size() > static_cast<std::size_t>(limits_.max_resources_per_plan)) {
    return handle;
  }
  if (request.obligations.size() > static_cast<std::size_t>(limits_.max_obligations_per_plan)) {
    return handle;
  }
  const PolicySlot* policy_slot = policy_slot_locked(request.policy_id);
  if (policy_slot == nullptr || !(policy_slot->policy.generation == request.policy_generation)) {
    return handle;
  }
  const RecoveryPolicy& pol = policy_slot->policy;

  if (!request.constrained.structurally_valid() || !request.rollback_target.structurally_valid() ||
      !request.baseline.structurally_valid()) {
    return handle;
  }

  RecoveryPlan plan{};
  plan.policy_id = request.policy_id;
  plan.policy_generation = request.policy_generation;
  plan.congestion = request.congestion;
  plan.intervention = request.intervention;
  plan.epoch = request.epoch;
  plan.resources = request.resources;
  plan.baseline = request.baseline;
  plan.baseline_known = request.baseline_known && !request.baseline.empty();
  plan.constrained = request.constrained;
  plan.rollback_target = request.rollback_target;
  plan.obligations = request.obligations;
  plan.created_at_tick = request.now_tick;
  plan.updated_at_tick = request.now_tick;
  plan.last_transition_tick = request.now_tick;

  {
    std::vector<ResourceId> seen;
    seen.reserve(plan.resources.size());
    for (const ResourceBinding& b : plan.resources) {
      if (!b.resource.is_valid() || !b.generation.is_valid()) {
        return handle;
      }
      seen.push_back(b.resource);
    }
    std::sort(seen.begin(), seen.end());
    if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
      return handle;
    }
    std::sort(plan.resources.begin(), plan.resources.end(),
              [](const ResourceBinding& a, const ResourceBinding& b) { return a.resource < b.resource; });
  }

  if (pol.baseline_required && !plan.baseline_known) {
    return handle;
  }

  // Every level vector must describe exactly the bound resource set: an extra
  // entry would be silently ignored, and a missing one would be treated as zero.
  const auto exact_resource_set = [&](const RestorationVector& v) {
    if (v.size() != plan.resources.size()) {
      return false;
    }
    for (std::size_t i = 0; i < plan.resources.size(); ++i) {
      const RestorationAmount* amount = v.at(i);
      if (amount == nullptr || !(amount->resource == plan.resources[i].resource)) {
        return false;
      }
    }
    return true;
  };
  if (!exact_resource_set(plan.constrained) || !exact_resource_set(plan.rollback_target)) {
    return handle;
  }
  if (plan.baseline_known && !exact_resource_set(plan.baseline)) {
    return handle;
  }
  if (!plan.baseline_known && !plan.baseline.empty()) {
    return handle;
  }
  for (const StageSpec& stage : pol.stages) {
    if (!exact_resource_set(stage.target)) {
      return handle;
    }
  }

  // The ladder opens on the constrained state: OBSERVE must not change levels.
  if (!pol.stages.front().target.equals(plan.constrained)) {
    return handle;
  }

  for (const ResourceBinding& b : plan.resources) {
    const RestorationAmount* constrained = plan.constrained.find(b.resource);
    const RestorationAmount* rollback = plan.rollback_target.find(b.resource);
    if (constrained == nullptr || rollback == nullptr) {
      return handle;
    }
    if (!is_valid(constrained->unit) || rollback->unit != constrained->unit) {
      return handle;
    }
    if (plan.baseline_known) {
      const RestorationAmount* base = plan.baseline.find(b.resource);
      if (base == nullptr || base->unit != constrained->unit) {
        return handle;
      }
    }
    std::uint64_t previous_target = 0;
    bool first_stage = true;
    for (const StageSpec& stage : pol.stages) {
      const RestorationAmount* target = stage.target.find(b.resource);
      if (target == nullptr || target->unit != constrained->unit) {
        return handle;
      }
      if (!first_stage && target->effective < previous_target) {
        return handle;  // restoration never decreases along the ladder
      }
      if (plan.baseline_known) {
        const RestorationAmount* base = plan.baseline.find(b.resource);
        if (target->effective > base->effective) {
          return handle;  // never restore above the pre-intervention baseline
        }
      }
      previous_target = target->effective;
      first_stage = false;
    }
    const RestorationAmount* base = plan.baseline_known ? plan.baseline.find(b.resource) : nullptr;
    const std::uint64_t envelope =
        base != nullptr ? max_u64(base->effective, constrained->effective) : constrained->effective;
    if (rollback->effective > envelope) {
      return handle;
    }
    if (rollback->effective > pol.stages.front().target.find(b.resource)->effective) {
      return handle;  // rollback target must not exceed the constrained state
    }
  }

  {
    std::vector<ObligationId> ids;
    ids.reserve(plan.obligations.size());
    for (const ProtectedObligation& o : plan.obligations) {
      if (!o.id.is_valid() || !o.generation.is_valid() || !is_valid(o.kind) || !is_valid(o.unit)) {
        return handle;
      }
      if (!plan.has_resource(o.resource)) {
        return handle;
      }
      const RestorationAmount* constrained = plan.constrained.find(o.resource);
      if (constrained == nullptr || constrained->unit != o.unit) {
        return handle;
      }
      ids.push_back(o.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
      return handle;
    }
    std::string detail;
    if (!plan_obligations_hold(plan, plan.rollback_target, detail)) {
      return handle;  // protected obligations must survive the rollback target
    }
    if (!plan_obligations_hold(plan, plan.constrained, detail)) {
      return handle;
    }
  }

  if (next_plan_id_ == kU64Max) {
    return handle;
  }
  plan.id = RecoveryPlanId(next_plan_id_);
  plan.generation = PlanGeneration(1);
  plan.stage_generation = StageGeneration(1);
  plan.stage_index = 0;
  plan.state = state_for_stage(pol.stages.front().kind);
  plan.attempt = AttemptId(1);
  plan.total_attempts = 1;
  plan.attempts_in_stage = 1;
  plan.authorized = plan.constrained;

  auto slot = std::make_unique<PlanSlot>();
  slot->plan = plan;
  {
    StageRecord& record = open_stage_record(slot->plan, pol.stages.front(), request.now_tick);
    record.requested = pol.stages.front().target;
    record.authorized = plan.constrained;
    record.epoch = plan.epoch;
  }
  const RecoveryPlanId new_id = plan.id;
  const PlanGeneration new_generation = plan.generation;
  const StageGeneration new_stage_generation = plan.stage_generation;
  plans_.emplace(new_id.value(), std::move(slot));
  if (new_id.value() >= next_plan_id_ && new_id.value() < kU64Max) {
    next_plan_id_ = new_id.value() + 1u;
  }

  LineageEvent event{};
  event.kind = "PLAN_CREATED";
  event.plan = new_id;
  event.plan_generation = new_generation;
  event.epoch = epoch_;
  event.tick = request.now_tick;
  event.detail = "plan created for intervention";
  if (lineage_.size() < static_cast<std::size_t>(limits_.max_adjacent_requests)) {
    lineage_.push_back(event);
  }

  handle.id = new_id;
  handle.generation = new_generation;
  handle.stage_generation = new_stage_generation;
  return handle;
}

bool RecoveryEngine::has_plan(RecoveryPlanId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return plan_slot_locked(id) != nullptr;
}

const RecoveryPlan* RecoveryEngine::plan(RecoveryPlanId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const PlanSlot* slot = plan_slot_locked(id);
  return slot != nullptr ? &slot->plan : nullptr;
}

std::vector<RecoveryPlanId> RecoveryEngine::plan_ids() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<RecoveryPlanId> out;
  out.reserve(plans_.size());
  for (const auto& entry : plans_) {
    out.push_back(entry.second->plan.id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool RecoveryEngine::erase_plan(RecoveryPlanId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = plans_.find(id.value());
  if (it == plans_.end()) {
    return false;
  }
  if (is_live(it->second->plan.state)) {
    return false;  // live plans are never discarded silently
  }
  plans_.erase(it);
  return true;
}

// ---------------------------------------------------------------------------
// Epoch and authority
// ---------------------------------------------------------------------------
CoordinatorEpoch RecoveryEngine::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return epoch_;
}

CoordinatorEpoch RecoveryEngine::begin_epoch(CoordinatorEpoch requested) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!requested.is_valid() || requested <= epoch_) {
    return epoch_;  // epochs never regress
  }
  epoch_ = requested;
  authority_.clear();  // live authority never survives an epoch change
  for (auto& entry : plans_) {
    RecoveryPlan& plan = entry.second->plan;
    if (!(plan.epoch == epoch_)) {
      invalidate_plan_locked(*entry.second, RejectReason::STALE_EPOCH, "coordinator epoch advanced");
    }
  }
  return epoch_;
}

bool RecoveryEngine::grant_authority(const RecoveryAuthority& grant) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!(grant.epoch == epoch_)) {
    return false;
  }
  return authority_.put(grant);
}

bool RecoveryEngine::revoke_authority(ResourceId resource) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (authority_.find(resource) == nullptr) {
    return false;
  }
  authority_.remove(resource);
  return true;
}

std::size_t RecoveryEngine::drop_stale_authority(std::uint64_t now_tick) {
  std::lock_guard<std::mutex> guard(mutex_);
  return authority_.drop_stale(epoch_, now_tick);
}

AuthorityVector RecoveryEngine::authority_snapshot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return authority_;
}

// ---------------------------------------------------------------------------
// Evidence evaluation
// ---------------------------------------------------------------------------
bool RecoveryEngine::evaluate_requirements_locked(const RecoveryPlan& plan, const RecoveryPolicy& policy,
                                                  const EvidenceSnapshot& snapshot,
                                                  std::vector<EvidenceEvaluation>& evaluations,
                                                  bool& contradictory, RejectReason& reason,
                                                  std::string& detail) const {
  evaluations.clear();
  contradictory = false;
  reason = RejectReason::NONE;
  detail.clear();

  const StageSpec* stage = stage_at(policy, plan.stage_index);
  if (stage == nullptr) {
    reason = RejectReason::NO_POLICY_STAGE;
    detail = "stage index outside policy";
    return false;
  }

  {
    std::vector<const Evidence*> ordered;
    ordered.reserve(snapshot.entries.size());
    for (const Evidence& e : snapshot.entries) {
      ordered.push_back(&e);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Evidence* a, const Evidence* b) {
      if (a->kind != b->kind) {
        return static_cast<std::uint8_t>(a->kind) < static_cast<std::uint8_t>(b->kind);
      }
      if (!(a->resource == b->resource)) {
        return a->resource < b->resource;
      }
      return a->generation < b->generation;
    });
    for (std::size_t i = 1; i < ordered.size(); ++i) {
      const Evidence& a = *ordered[i - 1];
      const Evidence& b = *ordered[i];
      if (a.kind != b.kind || !(a.resource == b.resource)) {
        continue;
      }
      if (!a.generation.is_valid() || !(a.generation == b.generation)) {
        continue;
      }
      if (a.value != b.value || a.unit != b.unit || a.affirmative != b.affirmative) {
        contradictory = true;
        reason = RejectReason::EVIDENCE_CONTRADICTORY;
        detail = "evidence generation ";
        detail += std::to_string(a.generation.value());
        detail += " reports conflicting values for ";
        detail += to_string(a.kind);
        return false;
      }
    }
  }

  const auto usable = [&](const Evidence& e) -> bool {
    if (!is_valid(e.kind)) {
      return false;
    }
    if (e.provenance == Provenance::UNKNOWN) {
      return false;
    }
    // Written as a negation so a NaN confidence (for which every comparison is
    // false) is rejected rather than silently treated as in range.
    if (!(e.confidence >= -kConfidenceEpsilon && e.confidence <= 1.0 + kConfidenceEpsilon)) {
      return false;
    }
    if (e.plan_generation.is_valid() && !(e.plan_generation == plan.generation)) {
      return false;
    }
    if (e.policy_generation.is_valid() && !(e.policy_generation == plan.policy_generation)) {
      return false;
    }
    if (e.epoch.is_valid() && !(e.epoch == epoch_)) {
      return false;
    }
    if (e.observed_at_tick > snapshot.assembled_at_tick) {
      return false;  // a future observation proves nothing about now
    }
    if (snapshot.assembled_at_tick - e.observed_at_tick > policy.max_evidence_age_ticks) {
      return false;
    }
    if (e.duration_ticks > snapshot.assembled_at_tick) {
      return false;
    }
    if (e.sample_count == 0u) {
      return false;
    }
    return true;
  };

  bool all_satisfied = true;
  for (const EvidenceRequirement& req : stage->requirements) {
    EvidenceEvaluation ev{};
    ev.kind = req.kind;
    const Evidence* latest = nullptr;
    for (const Evidence& e : snapshot.entries) {
      if (e.kind != req.kind) {
        continue;
      }
      if (req.require_bound_resource && !plan.has_resource(e.resource)) {
        continue;
      }
      if (!usable(e)) {
        continue;
      }
      const bool first = !ev.any_fresh;
      ev.any_fresh = true;
      ev.samples = add_u64(ev.samples, e.sample_count).value;
      ev.duration_ticks = max_u64(ev.duration_ticks, e.duration_ticks);
      ev.confidence = std::max(ev.confidence, e.confidence);
      if (provenance_rank(e.provenance) > provenance_rank(ev.best_provenance)) {
        ev.best_provenance = e.provenance;
      }
      ev.stable = first ? e.stable : (ev.stable && e.stable);
      if (latest == nullptr || e.observed_at_tick > latest->observed_at_tick ||
          (e.observed_at_tick == latest->observed_at_tick && latest->id < e.id)) {
        latest = &e;
      }
    }
    if (latest != nullptr) {
      ev.observed_value = latest->value;
      ev.latest_affirmative = latest->affirmative;
    }

    const std::uint64_t required_duration =
        req.require_stable ? max_u64(req.min_duration_ticks, req.stability_window_ticks)
                           : req.min_duration_ticks;
    bool ok = ev.any_fresh;
    if (ok) {
      ok = ev.samples >= req.min_samples;
    }
    if (ok) {
      ok = ev.duration_ticks >= required_duration;
    }
    if (ok) {
      ok = ev.confidence + kConfidenceEpsilon >= req.min_confidence;
    }
    if (ok) {
      ok = provenance_at_least(ev.best_provenance, req.min_provenance);
    }
    if (ok && req.require_stable) {
      ok = ev.stable;
    }
    if (ok && req.require_affirmative) {
      ok = ev.latest_affirmative;
    }
    if (ok && req.has_min_value) {
      ok = ev.observed_value >= req.min_value;
    }
    if (ok && req.has_max_value) {
      ok = ev.observed_value <= req.max_value;
    }
    ev.satisfied = ok;

    std::string text;
    text += to_string(req.kind);
    text += ok ? " satisfied" : " unsatisfied";
    text += " samples=";
    text += std::to_string(ev.samples);
    text += "/";
    text += std::to_string(req.min_samples);
    text += " duration=";
    text += std::to_string(ev.duration_ticks);
    text += "/";
    text += std::to_string(required_duration);
    text += " value=";
    text += std::to_string(ev.observed_value);
    text += " provenance=";
    text += to_string(ev.best_provenance);
    text += " stable=";
    text += ev.stable ? "yes" : "no";
    ev.detail = bound_text(text, limits_.max_reason_length);
    evaluations.push_back(ev);

    if (!ok) {
      all_satisfied = false;
      if (reason == RejectReason::NONE) {
        reason = RejectReason::EVIDENCE_INSUFFICIENT;
        detail = ev.detail;
      }
    }
  }
  return all_satisfied;
}

RestorationVector RecoveryEngine::collect_observed_locked(const RecoveryPlan& plan,
                                                          const RecoveryPolicy& policy,
                                                          const EvidenceSnapshot& snapshot,
                                                          const RestorationVector& candidate) const {
  RestorationVector out{};
  for (const RestorationAmount& target : candidate.entries()) {
    const Evidence* latest = nullptr;
    for (const Evidence& e : snapshot.entries) {
      if (e.kind != EvidenceKind::APPLIED_RESTORATION) {
        continue;
      }
      if (!(e.resource == target.resource)) {
        continue;
      }
      if (e.provenance == Provenance::UNKNOWN) {
        continue;
      }
      if (e.plan_generation.is_valid() && !(e.plan_generation == plan.generation)) {
        continue;
      }
      if (e.epoch.is_valid() && !(e.epoch == epoch_)) {
        continue;
      }
      if (e.observed_at_tick > snapshot.assembled_at_tick) {
        continue;
      }
      if (snapshot.assembled_at_tick - e.observed_at_tick > policy.max_evidence_age_ticks) {
        continue;
      }
      if (latest == nullptr || e.observed_at_tick > latest->observed_at_tick ||
          (e.observed_at_tick == latest->observed_at_tick && latest->id < e.id)) {
        latest = &e;
      }
    }
    if (latest == nullptr || latest->unit != target.unit) {
      continue;  // no observed level: reported unknown, never guessed
    }
    RestorationAmount amount{};
    amount.resource = target.resource;
    amount.unit = target.unit;
    amount.requested = target.effective;
    amount.authorized = target.effective;
    amount.effective = target.effective;
    amount.observed = latest->value;
    amount.observed_known = true;
    (void)out.put(amount);
  }
  return out;
}

bool RecoveryEngine::build_candidate_locked(const RecoveryPlan& plan, const StageSpec& stage,
                                            std::uint64_t now_tick, RestorationVector& candidate,
                                            std::vector<ObligationEvaluation>& obligations,
                                            std::vector<AuthorityView>& authority, RejectReason& reason,
                                            std::string& detail, bool& clamped_authority) const {
  candidate = stage.target;
  obligations.clear();
  authority.clear();
  reason = RejectReason::NONE;
  detail.clear();
  clamped_authority = false;

  for (const ResourceBinding& binding : plan.resources) {
    RestorationAmount* amount = candidate.find_mut(binding.resource);
    if (amount == nullptr) {
      continue;
    }
    AuthorityView view{};
    view.resource = binding.resource;
    view.unit = amount->unit;
    const RecoveryAuthority* grant = authority_.find(binding.resource);
    if (grant == nullptr) {
      view.present = false;
      authority.push_back(view);
      if (stage.requires_authority) {
        reason = RejectReason::AUTHORITY_MISSING;
        detail = "no live authority for resource ";
        detail += resource_text(binding.resource);
        return false;
      }
      amount->authorized = amount->requested;
      amount->effective = amount->requested;
      continue;
    }
    view.present = true;
    view.generation = grant->generation;
    view.ceiling = grant->ceiling;
    view.floor = grant->floor;
    view.epoch_match = grant->epoch == epoch_;
    view.expired = grant->expires_at_tick <= now_tick;
    authority.push_back(view);

    if (!stage.requires_authority) {
      amount->authorized = amount->requested;
      amount->effective = amount->requested;
      continue;
    }
    if (!view.epoch_match) {
      reason = RejectReason::STALE_EPOCH;
      detail = "authority bound to an older epoch";
      return false;
    }
    if (view.expired) {
      reason = RejectReason::AUTHORITY_EXPIRED;
      detail = "authority expired for resource ";
      detail += resource_text(binding.resource);
      return false;
    }
    if (grant->unit != amount->unit) {
      reason = RejectReason::INVALID_INPUT;
      detail = "authority unit mismatch for resource ";
      detail += resource_text(binding.resource);
      return false;
    }
    amount->authorized = clamp_u64(amount->requested, grant->floor, grant->ceiling);
    if (amount->authorized != amount->requested) {
      amount->authority_clamped = true;
      clamped_authority = true;
    }
    amount->effective = amount->authorized;
  }

  for (const ProtectedObligation& o : plan.obligations) {
    const ObligationEvaluation ev = evaluate_obligation(o, candidate);
    obligations.push_back(ev);
    if (ev.blocking) {
      reason = RejectReason::OBLIGATION_VIOLATION;
      detail = bound_text(ev.detail, limits_.max_reason_length);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Decision computation
// ---------------------------------------------------------------------------
StageDecision RecoveryEngine::compute_decision_locked(RecoveryPlan& draft,
                                                      const EvidenceSnapshot& snapshot) const {
  StageDecision d{};
  d.plan = draft.id;
  d.plan_generation = draft.generation;
  d.epoch = draft.epoch;
  d.state_before = draft.state;
  d.stage_index_before = draft.stage_index;
  d.stage_generation = draft.stage_generation;
  d.attempt = draft.attempt;
  d.tick = snapshot.assembled_at_tick;

  const auto settle = [&]() {
    d.state_after = draft.state;
    d.stage_index_after = draft.stage_index;
    return d;
  };
  const auto reject = [&](RecoveryDecision decision, RejectReason why, const std::string& text) {
    d.decision = decision;
    d.reason = why;
    d.reason_text = bound_text(text, limits_.max_reason_length);
    return settle();
  };

  const PolicySlot* policy_slot = policy_slot_locked(draft.policy_id);
  if (policy_slot == nullptr) {
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_POLICY, "bound policy is absent");
  }
  const RecoveryPolicy& pol = policy_slot->policy;
  const StageSpec* stage = stage_at(pol, draft.stage_index);
  if (stage == nullptr) {
    return reject(RecoveryDecision::REJECT_EVIDENCE, RejectReason::NO_POLICY_STAGE,
                  "stage index outside policy");
  }
  d.stage_kind_before = stage->kind;

  const std::uint64_t now = snapshot.assembled_at_tick;

  // 0. Replaying an exact completion is idempotent even when the plan has
  // already reached its terminal state: the recorded outcome is returned
  // unchanged and no authoritative state is touched.
  if (stage->kind == StageKind::COMPLETE && (draft.epoch == epoch_) &&
      (!snapshot.epoch.is_valid() || snapshot.epoch == epoch_)) {
    const std::uint64_t replay_key = completion_key(draft.id, draft.generation, draft.stage_generation,
                                                    snapshot.generation.value(), draft.attempt);
    for (const CompletionRecord& record : draft.completions) {
      if (record.idempotency_key != replay_key) {
        continue;
      }
      d.duplicate = true;
      d.idempotency_key = replay_key;
      d.completion = record.id;
      d.completion_generation = record.generation;
      d.stage_kind_after = StageKind::COMPLETE;
      if (record.state == CompletionState::COMMITTED) {
        d.decision = RecoveryDecision::COMPLETE;
        d.completed = true;
        d.reason = RejectReason::NONE;
        d.reason_text = "duplicate completion is idempotent";
        return settle();
      }
      d.decision = RecoveryDecision::REJECT_EVIDENCE;
      d.reason = record.reject != RejectReason::NONE ? record.reject : RejectReason::COMPLETION_FAILED;
      d.reason_text = "a failed completion can never be reused";
      return settle();
    }
  }

  // 1. epoch, policy and revalidation boundaries. These are checked before
  // liveness so a stale plan reports the real reason it is unusable.
  if (!(draft.epoch == epoch_)) {
    invalidate_draft(draft, RejectReason::STALE_EPOCH, "coordinator epoch advanced", limits_.max_reason_length);
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_EPOCH,
                  "plan bound to an older coordinator epoch");
  }
  if (!(draft.policy_generation == pol.generation)) {
    invalidate_draft(draft, RejectReason::STALE_POLICY, "policy generation advanced", limits_.max_reason_length);
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_POLICY,
                  "plan bound to an older policy generation");
  }
  if (draft.revalidation_required) {
    return reject(RecoveryDecision::REVALIDATE, RejectReason::REVALIDATION_BOUNDARY,
                  "revalidation is required before advancing");
  }

  // 2. terminal plans never change state.
  if (!is_live(draft.state)) {
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_LIVE;
    d.reason_text = "plan is terminal";
    return settle();
  }
  if (snapshot.epoch.is_valid() && !(snapshot.epoch == epoch_)) {
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_EPOCH,
                  "evidence snapshot bound to an older epoch");
  }
  if (snapshot.plan_generation.is_valid() && !(snapshot.plan_generation == draft.generation)) {
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_PLAN,
                  "evidence snapshot bound to an older plan generation");
  }
  if (snapshot.policy_generation.is_valid() && !(snapshot.policy_generation == draft.policy_generation)) {
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_POLICY,
                  "evidence snapshot bound to an older policy generation");
  }

  // 3. time coherence.
  if (now < draft.updated_at_tick) {
    return reject(RecoveryDecision::REJECT_STALE, RejectReason::STALE_EVIDENCE,
                  "evidence snapshot predates the current plan state");
  }
  if (now - draft.created_at_tick > pol.max_plan_lifetime_ticks) {
    draft.state = RecoveryState::FAILED;
    draft.last_reason = RejectReason::PLAN_LIFETIME_EXCEEDED;
    draft.last_reason_text = "plan lifetime exceeded";
    return reject(RecoveryDecision::FAIL, RejectReason::PLAN_LIFETIME_EXCEEDED, "plan lifetime exceeded");
  }
  if (now - draft.last_transition_tick > pol.revalidation_boundary_ticks) {
    invalidate_draft(draft, RejectReason::REVALIDATION_BOUNDARY,
                     "revalidation boundary crossed without fresh evidence", limits_.max_reason_length);
    return reject(RecoveryDecision::REVALIDATE, RejectReason::REVALIDATION_BOUNDARY,
                  "revalidation boundary crossed without fresh evidence");
  }

  if (draft.state == RecoveryState::ROLLING_BACK) {
    return reject(RecoveryDecision::HOLD, RejectReason::RECURRENCE_DETECTED,
                  "rollback in progress; compensating plan not yet applied");
  }
  if (draft.state == RecoveryState::PAUSED) {
    return reject(RecoveryDecision::HOLD, RejectReason::NOT_LIVE, "plan is paused; resume is required");
  }

  // 4. evidence.
  std::vector<EvidenceEvaluation> evaluations;
  bool contradictory = false;
  RejectReason evidence_reason = RejectReason::NONE;
  std::string evidence_detail;
  const bool all_satisfied = evaluate_requirements_locked(draft, pol, snapshot, evaluations, contradictory,
                                                          evidence_reason, evidence_detail);
  d.evidence = evaluations;
  if (contradictory) {
    return reject(RecoveryDecision::REJECT_EVIDENCE, RejectReason::EVIDENCE_CONTRADICTORY, evidence_detail);
  }

  const EvidenceRequirement* primary_req = nullptr;
  const EvidenceEvaluation* primary_ev = nullptr;
  for (std::size_t i = 0; i < stage->requirements.size() && i < evaluations.size(); ++i) {
    if (stage->requirements[i].primary) {
      primary_req = &stage->requirements[i];
      primary_ev = &evaluations[i];
    }
  }
  if (primary_req == nullptr || primary_ev == nullptr) {
    return reject(RecoveryDecision::REJECT_EVIDENCE, RejectReason::NO_POLICY_STAGE,
                  "stage has no primary requirement");
  }
  if (!primary_ev->any_fresh) {
    return reject(RecoveryDecision::REJECT_EVIDENCE, RejectReason::EVIDENCE_INSUFFICIENT,
                  "primary evidence is missing, stale or unusable");
  }

  // 5. hysteresis band and recurrence.
  bool in_advance_band = true;
  if (primary_req->has_min_value) {
    const std::uint64_t advance_floor = tick_add(primary_req->min_value, pol.hysteresis.advance_margin);
    const std::uint64_t rollback_floor = primary_req->min_value - pol.hysteresis.rollback_margin;
    if (primary_ev->observed_value < rollback_floor) {
      std::string text = "recurrence: ";
      text += to_string(primary_req->kind);
      text += "=";
      text += std::to_string(primary_ev->observed_value);
      text += " below rollback floor ";
      text += std::to_string(rollback_floor);
      text = bound_text(text, limits_.max_reason_length);
      draft.last_reason = RejectReason::RECURRENCE_DETECTED;
      draft.last_reason_text = text;
      switch (pol.on_recurrence) {
        case RecurrenceAction::PAUSE:
          draft.state = RecoveryState::PAUSED;
          return reject(RecoveryDecision::PAUSE, RejectReason::RECURRENCE_DETECTED, text);
        case RecurrenceAction::FAIL:
          draft.state = RecoveryState::FAILED;
          return reject(RecoveryDecision::FAIL, RejectReason::RECURRENCE_DETECTED, text);
        case RecurrenceAction::ROLLBACK:
        default:
          draft.state = RecoveryState::ROLLING_BACK;
          return reject(RecoveryDecision::ROLLBACK, RejectReason::RECURRENCE_DETECTED, text);
      }
    }
    if (primary_ev->observed_value < advance_floor) {
      in_advance_band = false;
    }
  }
  if (!in_advance_band) {
    std::string text = "evidence inside the hold band for ";
    text += to_string(primary_req->kind);
    text += " value=";
    text += std::to_string(primary_ev->observed_value);
    return reject(RecoveryDecision::HOLD, RejectReason::HYSTERESIS_BAND, text);
  }

  // 6. dwell, cooldown and transition rate.
  if (now < draft.cooldown_until_tick) {
    const std::uint64_t remaining = draft.cooldown_until_tick - now;
    std::string text = "cooldown active; ";
    text += std::to_string(remaining);
    text += " ticks remain";
    return reject(RecoveryDecision::HOLD, RejectReason::COOLDOWN_ACTIVE, text);
  }
  if (now - draft.last_transition_tick < stage->dwell_ticks) {
    const std::uint64_t remaining = stage->dwell_ticks - (now - draft.last_transition_tick);
    std::string text = "dwell not satisfied; ";
    text += std::to_string(remaining);
    text += " ticks remain";
    return reject(RecoveryDecision::HOLD, RejectReason::DWELL_NOT_SATISFIED, text);
  }
  if (count_recent(draft.transition_ticks, now, pol.hysteresis.window_ticks) >=
      static_cast<std::size_t>(pol.hysteresis.max_transitions_per_window)) {
    draft.state = RecoveryState::PAUSED;
    draft.last_reason = RejectReason::TRANSITION_RATE_EXCEEDED;
    draft.last_reason_text = "transition rate exceeds bounded hysteresis";
    return reject(RecoveryDecision::PAUSE, RejectReason::TRANSITION_RATE_EXCEEDED,
                  "transition rate exceeds bounded hysteresis");
  }

  // 7. attempt budget.
  if (draft.total_attempts >= pol.max_total_attempts) {
    draft.state = RecoveryState::FAILED;
    draft.last_reason = RejectReason::ATTEMPT_BUDGET_EXHAUSTED;
    draft.last_reason_text = "total attempt budget exhausted";
    return reject(RecoveryDecision::FAIL, RejectReason::ATTEMPT_BUDGET_EXHAUSTED,
                  "total attempt budget exhausted");
  }
  if (draft.attempts_in_stage >= stage->max_attempts) {
    const std::string text = "stage attempt budget exhausted";
    draft.last_reason = RejectReason::ATTEMPT_BUDGET_EXHAUSTED;
    draft.last_reason_text = text;
    if (stage->rollback_on_failure) {
      draft.state = RecoveryState::ROLLING_BACK;
      return reject(RecoveryDecision::ROLLBACK, RejectReason::ATTEMPT_BUDGET_EXHAUSTED, text);
    }
    draft.state = RecoveryState::FAILED;
    return reject(RecoveryDecision::FAIL, RejectReason::ATTEMPT_BUDGET_EXHAUSTED, text);
  }

  if (!all_satisfied) {
    return reject(RecoveryDecision::HOLD, RejectReason::EVIDENCE_INSUFFICIENT, evidence_detail);
  }

  // 8. candidate levels under authority and protected obligations.
  RestorationVector candidate{};
  std::vector<ObligationEvaluation> obligations;
  std::vector<AuthorityView> authority;
  RejectReason candidate_reason = RejectReason::NONE;
  std::string candidate_detail;
  bool clamped = false;
  if (!build_candidate_locked(draft, *stage, now, candidate, obligations, authority, candidate_reason,
                              candidate_detail, clamped)) {
    d.obligations = obligations;
    d.authority = authority;
    d.effective = candidate;
    const RecoveryDecision decision = candidate_reason == RejectReason::OBLIGATION_VIOLATION
                                          ? RecoveryDecision::REJECT_OBLIGATION
                                          : RecoveryDecision::REJECT_AUTHORITY;
    return reject(decision, candidate_reason, candidate_detail);
  }
  d.obligations = obligations;
  d.authority = authority;
  d.effective = candidate;
  d.clamped_by_authority = clamped;

  // 9. terminal-stage verification and completion idempotency.
  RestorationVector observed{};
  const std::uint64_t key = completion_key(draft.id, draft.generation, draft.stage_generation,
                                           snapshot.generation.value(), draft.attempt);
  if (stage->kind == StageKind::COMPLETE) {
    observed = collect_observed_locked(draft, pol, snapshot, candidate);
    d.observed = observed;
    bool partial = false;
    std::string partial_detail;
    if (pol.require_completion_evidence) {
      for (const RestorationAmount& target : candidate.entries()) {
        const RestorationAmount* seen = observed.find(target.resource);
        const std::uint64_t required =
            pct_bp_u64(target.effective, 10000u - pol.partial_application_tolerance_bp).value;
        if (seen == nullptr || !seen->observed_known) {
          partial = true;
          partial_detail = "no observed applied level for resource ";
          partial_detail += resource_text(target.resource);
          break;
        }
        if (seen->observed < required) {
          partial = true;
          partial_detail = "partial application on resource ";
          partial_detail += resource_text(target.resource);
          partial_detail += ": observed ";
          partial_detail += std::to_string(seen->observed);
          partial_detail += " below required ";
          partial_detail += std::to_string(required);
          break;
        }
      }
    }
    if (partial) {
      if (draft.completions.size() >= static_cast<std::size_t>(limits_.max_completion_history)) {
        draft.state = RecoveryState::FAILED;
        draft.last_reason = RejectReason::LIMIT_EXCEEDED;
        draft.last_reason_text = "completion history is full";
        return reject(RecoveryDecision::FAIL, RejectReason::LIMIT_EXCEEDED, "completion history is full");
      }
      CompletionRecord failed{};
      failed.id = CompletionId(draft.completions.size() + 1u);
      failed.generation = CompletionGeneration(1);
      failed.epoch = draft.epoch;
      failed.stage_index = draft.stage_index;
      failed.stage_generation = draft.stage_generation;
      failed.attempt = draft.attempt;
      failed.evidence_generation = snapshot_evidence_generation(snapshot);
      failed.state = CompletionState::FAILED;
      failed.recorded_tick = now;
      failed.idempotency_key = key;
      failed.applied = candidate;
      failed.reject = RejectReason::PARTIAL_APPLICATION;
      draft.completions.push_back(failed);
      draft.attempts_in_stage += 1;
      draft.total_attempts += 1;
      draft.last_reason = RejectReason::PARTIAL_APPLICATION;
      draft.last_reason_text = bound_text(partial_detail, limits_.max_reason_length);
      if (stage->rollback_on_failure) {
        draft.state = RecoveryState::ROLLING_BACK;
        return reject(RecoveryDecision::ROLLBACK, RejectReason::PARTIAL_APPLICATION, partial_detail);
      }
      return reject(RecoveryDecision::HOLD, RejectReason::PARTIAL_APPLICATION, partial_detail);
    }
    if (pol.require_stable_completion && !primary_ev->stable) {
      return reject(RecoveryDecision::HOLD, RejectReason::EVIDENCE_INSUFFICIENT,
                    "completion evidence is not stable");
    }
  }

  // 11. close the current stage lineage record.
  if (draft.stage_history.size() >= static_cast<std::size_t>(limits_.max_stage_history) &&
      pending_stage_record(draft) == nullptr) {
    draft.state = RecoveryState::FAILED;
    draft.last_reason = RejectReason::LIMIT_EXCEEDED;
    draft.last_reason_text = "stage history is full";
    return reject(RecoveryDecision::FAIL, RejectReason::LIMIT_EXCEEDED, "stage history is full");
  }
  {
    StageRecord& open_record = open_stage_record(draft, *stage, draft.last_transition_tick);
    open_record.outcome = StageOutcome::SUCCEEDED;
    open_record.exited_tick = now;
    open_record.evidence_generation = snapshot_evidence_generation(snapshot);
    open_record.authority_generation =
        authority.empty() ? AuthorityGeneration{} : authority.front().generation;
    open_record.epoch = draft.epoch;
    open_record.attempts = draft.attempts_in_stage;
    open_record.requested = stage->target;
    open_record.authorized = candidate;
    open_record.observed = observed;
  }

  if (stage->kind == StageKind::COMPLETE) {
    if (draft.completions.size() >= static_cast<std::size_t>(limits_.max_completion_history)) {
      draft.state = RecoveryState::FAILED;
      draft.last_reason = RejectReason::LIMIT_EXCEEDED;
      draft.last_reason_text = "completion history is full";
      return reject(RecoveryDecision::FAIL, RejectReason::LIMIT_EXCEEDED, "completion history is full");
    }
    CompletionRecord committed{};
    committed.id = CompletionId(draft.completions.size() + 1u);
    committed.generation = CompletionGeneration(1);
    committed.epoch = draft.epoch;
    committed.stage_index = draft.stage_index;
    committed.stage_generation = draft.stage_generation;
    committed.attempt = draft.attempt;
    committed.evidence_generation = snapshot_evidence_generation(snapshot);
    committed.state = CompletionState::COMMITTED;
    committed.recorded_tick = now;
    committed.idempotency_key = key;
    committed.applied = candidate;
    draft.completions.push_back(committed);
    draft.completion_count += 1;
    draft.state = RecoveryState::COMPLETED;
    draft.authorized = candidate;
    draft.updated_at_tick = now;
    draft.last_transition_tick = now;
    draft.last_observed = observed;
    draft.last_reason = RejectReason::NONE;
    draft.last_reason_text.clear();
    push_transition(draft, now);
    d.decision = RecoveryDecision::COMPLETE;
    d.completed = true;
    d.advanced = true;
    d.completion = committed.id;
    d.completion_generation = committed.generation;
    d.idempotency_key = key;
    d.stage_kind_after = StageKind::COMPLETE;
    d.reason = RejectReason::NONE;
    d.reason_text = "recovery completed with verified applied restoration";
    return settle();
  }

  if (draft.stage_index + 1u >= pol.stages.size()) {
    return reject(RecoveryDecision::FAIL, RejectReason::NO_POLICY_STAGE, "no further stage in policy");
  }

  draft.stage_index += 1u;
  const StageSpec* next_stage = stage_at(pol, draft.stage_index);
  if (next_stage == nullptr) {
    return reject(RecoveryDecision::FAIL, RejectReason::NO_POLICY_STAGE, "next stage is outside policy");
  }
  if (!draft.stage_generation.bump()) {
    draft.state = RecoveryState::FAILED;
    draft.last_reason = RejectReason::LIMIT_EXCEEDED;
    draft.last_reason_text = "stage generation exhausted";
    return reject(RecoveryDecision::FAIL, RejectReason::LIMIT_EXCEEDED, "stage generation exhausted");
  }
  draft.attempt = AttemptId(draft.total_attempts + 1u);
  draft.total_attempts += 1;
  draft.attempts_in_stage = 1u;
  draft.state = state_for_stage(next_stage->kind);
  draft.authorized = candidate;
  draft.updated_at_tick = now;
  draft.last_transition_tick = now;
  draft.last_reason = RejectReason::NONE;
  draft.last_reason_text.clear();
  push_transition(draft, now);
  {
    StageRecord& next_record = open_stage_record(draft, *next_stage, now);
    next_record.requested = next_stage->target;
    next_record.epoch = draft.epoch;
  }

  d.decision = RecoveryDecision::ADVANCE_STAGE;
  d.advanced = true;
  d.stage_kind_after = next_stage->kind;
  d.reason = RejectReason::NONE;
  d.reason_text = "stage advanced";
  return settle();
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------
StageDecision RecoveryEngine::evaluate_locked(RecoveryPlanId id, PlanGeneration generation,
                                              const EvidenceSnapshot& snapshot, bool apply) const {
  StageDecision d{};
  const PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    d.plan = id;
    d.plan_generation = generation;
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_FOUND;
    d.reason_text = "plan not found";
    return d;
  }
  if (!(slot->plan.generation == generation)) {
    d.plan = id;
    d.plan_generation = generation;
    d.decision = RecoveryDecision::REJECT_STALE;
    d.reason = RejectReason::GENERATION_MISMATCH;
    d.reason_text = "plan generation mismatch";
    d.epoch = slot->plan.epoch;
    d.state_before = slot->plan.state;
    d.state_after = slot->plan.state;
    return d;
  }
  RecoveryPlan draft = slot->plan;
  d = compute_decision_locked(draft, snapshot);
  if (apply) {
    PlanSlot* mutable_slot = plan_slot_locked(id);
    if (mutable_slot != nullptr) {
      mutable_slot->plan = std::move(draft);
    }
  }
  return d;
}

StageDecision RecoveryEngine::evaluate(RecoveryPlanId id, PlanGeneration generation,
                                       const EvidenceSnapshot& snapshot) const {
  return evaluate_locked(id, generation, snapshot, false);
}

StageDecision RecoveryEngine::advance(RecoveryPlanId id, PlanGeneration generation,
                                      const EvidenceSnapshot& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);
  ++evaluations_;
  const StageDecision d = evaluate_locked(id, generation, snapshot, true);
  if (d.advanced || d.completed) {
    ++advances_;
  }
  if (d.duplicate) {
    ++completion_replays_;
  }
  switch (d.reason) {
    case RejectReason::STALE_EPOCH:
    case RejectReason::STALE_PLAN:
    case RejectReason::STALE_POLICY:
    case RejectReason::STALE_EVIDENCE:
    case RejectReason::STALE_COMPLETION:
    case RejectReason::STALE_RESOURCE:
    case RejectReason::REVALIDATION_BOUNDARY:
    case RejectReason::GENERATION_MISMATCH:
      ++stale_rejections_;
      break;
    default:
      break;
  }
  const PlanSlot* slot = plan_slot_locked(id);
  if (slot != nullptr) {
    if (d.completed) {
      emit_locked("COMPLETION_COMMITTED", slot->plan, d.reason_text);
    } else if (d.advanced) {
      emit_locked("STAGE_ADVANCED", slot->plan, d.reason_text);
    } else if (d.decision == RecoveryDecision::ROLLBACK || d.decision == RecoveryDecision::FAIL ||
               d.decision == RecoveryDecision::REVALIDATE) {
      emit_locked(std::string(to_string(d.decision)), slot->plan, d.reason_text);
    }
  }
  return d;
}

CompletionOutcome RecoveryEngine::complete(RecoveryPlanId id, PlanGeneration generation,
                                           const EvidenceSnapshot& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);
  ++evaluations_;
  CompletionOutcome out{};
  const PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    out.reason = RejectReason::NOT_FOUND;
    out.reason_text = "plan not found";
    return out;
  }
  if (!(slot->plan.generation == generation)) {
    out.reason = RejectReason::GENERATION_MISMATCH;
    out.reason_text = "plan generation mismatch";
    out.state = CompletionState::REJECTED_STALE;
    out.plan_state = slot->plan.state;
    return out;
  }
  const PolicySlot* policy_slot = policy_slot_locked(slot->plan.policy_id);
  const StageSpec* stage =
      policy_slot != nullptr ? stage_at(policy_slot->policy, slot->plan.stage_index) : nullptr;
  if (stage == nullptr) {
    out.reason = RejectReason::NO_POLICY_STAGE;
    out.reason_text = "stage index outside policy";
    return out;
  }
  if (stage->kind != StageKind::COMPLETE) {
    out.reason = RejectReason::NOT_LIVE;
    out.reason_text = "completion requires the COMPLETE stage";
    out.plan_state = slot->plan.state;
    return out;
  }
  const StageDecision d = evaluate_locked(id, generation, snapshot, true);
  out.duplicate = d.duplicate;
  out.reason = d.reason;
  out.reason_text = d.reason_text;
  out.completion = d.completion;
  out.generation = d.completion_generation;
  out.idempotency_key = d.idempotency_key;
  out.plan_state = d.state_after;
  if (d.completed && d.duplicate) {
    out.ok = true;
    out.state = CompletionState::COMMITTED;
    ++completion_replays_;
  } else if (d.completed) {
    out.ok = true;
    out.state = CompletionState::COMMITTED;
    ++advances_;
    const PlanSlot* refreshed = plan_slot_locked(id);
    if (refreshed != nullptr) {
      emit_locked("COMPLETION_COMMITTED", refreshed->plan, d.reason_text);
    }
  } else if (d.duplicate) {
    ++completion_replays_;
    out.state = CompletionState::FAILED;
    if (out.reason == RejectReason::NONE) {
      out.reason = RejectReason::COMPLETION_FAILED;
      out.reason_text = "a failed completion can never be reused";
    }
  } else {
    out.state = CompletionState::FAILED;
    if (out.reason == RejectReason::NONE) {
      out.reason = RejectReason::COMPLETION_FAILED;
    }
    if (d.decision == RecoveryDecision::REJECT_STALE) {
      out.state = CompletionState::REJECTED_STALE;
    }
  }
  return out;
}

StageDecision RecoveryEngine::pause(RecoveryPlanId id, PlanGeneration generation, RejectReason reason,
                                    std::string text) {
  std::lock_guard<std::mutex> guard(mutex_);
  StageDecision d{};
  PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    d.plan = id;
    d.plan_generation = generation;
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_FOUND;
    d.reason_text = "plan not found";
    return d;
  }
  RecoveryPlan& plan = slot->plan;
  d.plan = plan.id;
  d.plan_generation = plan.generation;
  d.epoch = plan.epoch;
  d.state_before = plan.state;
  d.stage_index_before = plan.stage_index;
  d.stage_generation = plan.stage_generation;
  d.attempt = plan.attempt;
  if (!(plan.generation == generation)) {
    d.decision = RecoveryDecision::REJECT_STALE;
    d.reason = RejectReason::GENERATION_MISMATCH;
    d.reason_text = "plan generation mismatch";
    d.state_after = plan.state;
    return d;
  }
  if (!is_live(plan.state)) {
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_LIVE;
    d.reason_text = "plan is terminal";
    d.state_after = plan.state;
    return d;
  }
  if (plan.state == RecoveryState::ROLLING_BACK) {
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::RECURRENCE_DETECTED;
    d.reason_text = "rollback already in progress";
    d.state_after = plan.state;
    return d;
  }
  plan.state = RecoveryState::PAUSED;
  plan.last_reason = reason;
  plan.last_reason_text = bound_text(text, limits_.max_reason_length);
  d.decision = RecoveryDecision::PAUSE;
  d.reason = reason;
  d.reason_text = plan.last_reason_text;
  d.state_after = plan.state;
  d.stage_index_after = plan.stage_index;
  emit_locked("PLAN_PAUSED", plan, plan.last_reason_text);
  return d;
}

StageDecision RecoveryEngine::resume(RecoveryPlanId id, PlanGeneration generation, std::uint64_t now_tick) {
  std::lock_guard<std::mutex> guard(mutex_);
  StageDecision d{};
  PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    d.plan = id;
    d.plan_generation = generation;
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_FOUND;
    d.reason_text = "plan not found";
    return d;
  }
  RecoveryPlan& plan = slot->plan;
  d.plan = plan.id;
  d.plan_generation = plan.generation;
  d.epoch = plan.epoch;
  d.state_before = plan.state;
  d.stage_index_before = plan.stage_index;
  d.stage_generation = plan.stage_generation;
  d.attempt = plan.attempt;
  d.tick = now_tick;
  if (!(plan.generation == generation)) {
    d.decision = RecoveryDecision::REJECT_STALE;
    d.reason = RejectReason::GENERATION_MISMATCH;
    d.reason_text = "plan generation mismatch";
    d.state_after = plan.state;
    return d;
  }
  if (plan.state != RecoveryState::PAUSED && plan.state != RecoveryState::ROLLED_BACK) {
    d.decision = RecoveryDecision::NO_OP;
    d.reason = RejectReason::NOT_LIVE;
    d.reason_text = "plan is not paused or rolled back";
    d.state_after = plan.state;
    return d;
  }
  if (plan.revalidation_required) {
    d.decision = RecoveryDecision::REVALIDATE;
    d.reason = RejectReason::REVALIDATION_BOUNDARY;
    d.reason_text = "revalidation is required before resuming";
    d.state_after = plan.state;
    return d;
  }
  if (now_tick < plan.cooldown_until_tick) {
    d.decision = RecoveryDecision::HOLD;
    d.reason = RejectReason::COOLDOWN_ACTIVE;
    d.reason_text = "cooldown active";
    d.state_after = plan.state;
    return d;
  }
  const PolicySlot* policy_slot = policy_slot_locked(plan.policy_id);
  const StageSpec* stage =
      policy_slot != nullptr ? stage_at(policy_slot->policy, plan.stage_index) : nullptr;
  if (stage == nullptr) {
    d.decision = RecoveryDecision::FAIL;
    d.reason = RejectReason::NO_POLICY_STAGE;
    d.reason_text = "stage index outside policy";
    d.state_after = plan.state;
    return d;
  }
  plan.state = state_for_stage(stage->kind);
  if (plan.state == RecoveryState::UNKNOWN) {
    plan.state = RecoveryState::PLANNED;
  }
  plan.attempts_in_stage = 0;
  plan.last_reason = RejectReason::NONE;
  plan.last_reason_text.clear();
  plan.last_transition_tick = now_tick;
  plan.updated_at_tick = now_tick;
  d.decision = RecoveryDecision::RESUME;
  d.reason = RejectReason::NONE;
  d.reason_text = "plan resumed";
  d.state_after = plan.state;
  d.stage_index_after = plan.stage_index;
  emit_locked("PLAN_RESUMED", plan, d.reason_text);
  return d;
}

RollbackOutcome RecoveryEngine::rollback(const RollbackRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  RollbackOutcome out{};
  PlanSlot* slot = plan_slot_locked(request.plan);
  if (slot == nullptr) {
    out.reason = RejectReason::NOT_FOUND;
    out.reason_text = "plan not found";
    return out;
  }
  RecoveryPlan& plan = slot->plan;
  if (!(plan.generation == request.plan_generation)) {
    out.reason = RejectReason::GENERATION_MISMATCH;
    out.reason_text = "plan generation mismatch";
    out.state_after = plan.state;
    return out;
  }
  if (request.epoch.is_valid() && !(request.epoch == epoch_)) {
    out.reason = RejectReason::STALE_EPOCH;
    out.reason_text = "rollback requested under an older epoch";
    out.state_after = plan.state;
    return out;
  }
  if (!(plan.epoch == epoch_)) {
    invalidate_plan_locked(*slot, RejectReason::STALE_EPOCH, "plan bound to an older epoch");
    out.reason = RejectReason::STALE_EPOCH;
    out.reason_text = "plan bound to an older epoch";
    out.state_after = plan.state;
    return out;
  }
  if (!is_live(plan.state)) {
    out.reason = RejectReason::NOT_LIVE;
    out.reason_text = "plan is terminal";
    out.state_after = plan.state;
    return out;
  }
  if (provenance_rank(request.provenance) < provenance_rank(Provenance::REPORTED)) {
    out.reason = RejectReason::EVIDENCE_INSUFFICIENT;
    out.reason_text = "rollback requires observed state from a real observation";
    out.state_after = plan.state;
    return out;
  }
  if (request.observed_current.empty() || !request.observed_current.structurally_valid()) {
    out.reason = RejectReason::INVALID_INPUT;
    out.reason_text = "rollback requires a structurally valid, non-empty observation";
    out.state_after = plan.state;
    return out;
  }
  if (plan.rollback_history.size() >= static_cast<std::size_t>(limits_.max_rollback_history)) {
    plan.state = RecoveryState::FAILED;
    out.reason = RejectReason::LIMIT_EXCEEDED;
    out.reason_text = "rollback history is full";
    out.state_after = plan.state;
    return out;
  }

  const CompensatingPlan compensation =
      build_compensating_plan(request.observed_current, plan.rollback_target);

  RollbackRecord record{};
  record.id = RollbackId(plan.rollback_count + 1u);
  record.generation = RollbackGeneration(1);
  record.epoch = plan.epoch;
  record.kind = compensation.complete
                    ? (compensation.any_compensation ? RollbackKind::COMPENSATING : RollbackKind::HOLD_SAFE)
                    : RollbackKind::PARTIAL_COMPENSATION;
  record.trigger = request.trigger;
  record.tick = request.now_tick;
  record.observed_current = request.observed_current;
  record.target = plan.rollback_target;
  record.compensation = compensation.target;
  record.unobserved = compensation.unobserved;
  record.complete = compensation.complete;
  record.reason = bound_text(request.reason, limits_.max_reason_length);
  record.compensation_steps = static_cast<std::uint32_t>(compensation.steps.size());
  record.observed_steps = static_cast<std::uint32_t>(compensation.already_at_target.size());
  for (const RestorationAmount& authorized : plan.authorized.entries()) {
    const RestorationAmount* target = plan.rollback_target.find(authorized.resource);
    if (target == nullptr || authorized.effective <= target->effective) {
      continue;
    }
    const RestorationAmount* seen = request.observed_current.find(authorized.resource);
    if (seen == nullptr || !seen->observed_known) {
      // Requested earlier but never observed: this is exactly the assumption a
      // rollback must not make.
      record.assumed_steps += 1;
    }
  }

  const PolicySlot* rollback_policy = policy_slot_locked(plan.policy_id);
  const StageSpec* rollback_stage =
      rollback_policy != nullptr ? stage_at(rollback_policy->policy, plan.stage_index) : nullptr;
  if (rollback_stage != nullptr &&
      (plan.stage_history.size() < static_cast<std::size_t>(limits_.max_stage_history) ||
       pending_stage_record(plan) != nullptr)) {
    StageRecord& open_record = open_stage_record(plan, *rollback_stage, plan.last_transition_tick);
    open_record.outcome = StageOutcome::ROLLED_BACK;
    open_record.exited_tick = request.now_tick;
    open_record.evidence_generation = request.evidence_generation;
    open_record.epoch = plan.epoch;
    open_record.attempts = plan.attempts_in_stage;
    open_record.last_reason = request.trigger;
  }

  plan.rollback_history.push_back(record);
  plan.rollback_count += 1;
  plan.authorized = compensation.target;
  plan.last_observed = request.observed_current;
  plan.updated_at_tick = request.now_tick;
  plan.last_transition_tick = request.now_tick;
  plan.attempts_in_stage = 0;
  plan.cooldown_until_tick = request.now_tick;
  {
    const PolicySlot* policy_slot = policy_slot_locked(plan.policy_id);
    if (policy_slot != nullptr) {
      plan.cooldown_until_tick = tick_add(request.now_tick, policy_slot->policy.hysteresis.cooldown_ticks);
    }
  }
  push_transition(plan, request.now_tick);
  plan.state = RecoveryState::ROLLED_BACK;
  plan.last_reason = request.trigger;
  plan.last_reason_text = bound_text(request.reason, limits_.max_reason_length);

  const PolicySlot* policy_slot = policy_slot_locked(plan.policy_id);
  if (policy_slot != nullptr && plan.rollback_count >= policy_slot->policy.max_rollbacks) {
    plan.state = RecoveryState::FAILED;
    plan.last_reason = RejectReason::ROLLBACK_BUDGET_EXHAUSTED;
    plan.last_reason_text = "rollback budget exhausted";
    out.plan_failed = true;
  }

  ++rollbacks_performed_;
  out.ok = true;
  out.rollback = record.id;
  out.generation = record.generation;
  out.compensation = compensation;
  out.state_after = plan.state;
  out.reason = request.trigger;
  out.reason_text = plan.last_reason_text;
  emit_locked("ROLLBACK_RECORDED", plan, record.reason);
  return out;
}

// ---------------------------------------------------------------------------
// Invalidation and revalidation
// ---------------------------------------------------------------------------
void RecoveryEngine::invalidate_plan_locked(PlanSlot& slot, RejectReason reason, const std::string& detail) {
  RecoveryPlan& plan = slot.plan;
  if (!is_live(plan.state)) {
    // A terminal plan's history is already durable and idempotent. An epoch or
    // policy change does not retroactively un-complete it; stale work bound to
    // the old epoch is still refused by the epoch check.
    return;
  }
  plan.revalidation_required = true;
  plan.state = RecoveryState::REVALIDATION_REQUIRED;
  plan.last_reason = reason;
  plan.last_reason_text = bound_text(detail, limits_.max_reason_length);
  if (plan.revalidation_notes.size() < static_cast<std::size_t>(limits_.max_explanation_items)) {
    plan.revalidation_notes.push_back(plan.last_reason_text);
  }
}

std::size_t RecoveryEngine::notify_resource_change(ResourceId resource, ResourceGeneration generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!resource.is_valid() || !generation.is_valid()) {
    return 0;
  }
  std::size_t affected = 0;
  for (auto& entry : plans_) {
    RecoveryPlan& plan = entry.second->plan;
    const ResourceBinding* binding = plan.binding(resource);
    if (binding == nullptr || binding->generation >= generation) {
      continue;  // absent or a generation regression, which is not a change
    }
    for (ResourceBinding& b : plan.resources) {
      if (b.resource == resource) {
        b.generation = generation;
      }
    }
    invalidate_plan_locked(*entry.second, RejectReason::STALE_RESOURCE,
                           "bound resource generation advanced");
    ++affected;
  }
  return affected;
}

std::size_t RecoveryEngine::notify_policy_change(PolicyId id, PolicyGeneration generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!id.is_valid() || !generation.is_valid()) {
    return 0;
  }
  std::size_t affected = 0;
  for (auto& entry : plans_) {
    RecoveryPlan& plan = entry.second->plan;
    if (!(plan.policy_id == id) || plan.policy_generation >= generation) {
      continue;
    }
    invalidate_plan_locked(*entry.second, RejectReason::STALE_POLICY, "policy generation advanced");
    ++affected;
  }
  return affected;
}

bool RecoveryEngine::require_revalidation(RecoveryPlanId id, RejectReason reason, std::string detail) {
  std::lock_guard<std::mutex> guard(mutex_);
  PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    return false;
  }
  invalidate_plan_locked(*slot, reason, detail);
  slot->plan.attempts_in_stage = 0;  // no attempt carries over a lost worker
  emit_locked("REVALIDATION_REQUIRED", slot->plan, detail);
  return true;
}

RevalidationOutcome RecoveryEngine::revalidate(RecoveryPlanId id, PlanGeneration generation,
                                               PolicyGeneration policy_generation,
                                               const std::vector<ResourceBinding>& resources,
                                               std::uint64_t now_tick) {
  std::lock_guard<std::mutex> guard(mutex_);
  RevalidationOutcome out{};
  PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    out.reason = RejectReason::NOT_FOUND;
    out.reason_text = "plan not found";
    return out;
  }
  RecoveryPlan& plan = slot->plan;
  out.plan_state = plan.state;
  if (!(plan.generation == generation)) {
    out.reason = RejectReason::GENERATION_MISMATCH;
    out.reason_text = "plan generation mismatch";
    return out;
  }
  const PolicySlot* policy_slot = policy_slot_locked(plan.policy_id);
  if (policy_slot == nullptr) {
    out.reason = RejectReason::STALE_POLICY;
    out.reason_text = "bound policy is absent";
    return out;
  }
  if (!(policy_slot->policy.generation == policy_generation)) {
    out.reason = RejectReason::STALE_POLICY;
    out.reason_text = "revalidation policy generation does not match the stored policy";
    return out;
  }
  if (resources.size() != plan.resources.size()) {
    out.reason = RejectReason::STALE_RESOURCE;
    out.reason_text = "revalidation resource set does not match the plan";
    return out;
  }
  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (!(resources[i].resource == plan.resources[i].resource) || !resources[i].generation.is_valid() ||
        resources[i].generation < plan.resources[i].generation) {
      out.reason = RejectReason::STALE_RESOURCE;
      out.reason_text = "revalidation resource binding regressed or changed identity";
      return out;
    }
  }
  const StageSpec* stage = stage_at(policy_slot->policy, plan.stage_index);
  if (stage == nullptr) {
    plan.state = RecoveryState::FAILED;
    out.reason = RejectReason::NO_POLICY_STAGE;
    out.reason_text = "stage index outside policy";
    out.plan_state = plan.state;
    return out;
  }
  if (!is_live(plan.state) || plan.state == RecoveryState::ROLLING_BACK) {
    out.reason = RejectReason::NOT_LIVE;
    out.reason_text = "plan cannot be revalidated in its current state";
    return out;
  }
  if (plan.stage_history.size() >= static_cast<std::size_t>(limits_.max_stage_history)) {
    plan.state = RecoveryState::FAILED;
    out.reason = RejectReason::LIMIT_EXCEEDED;
    out.reason_text = "stage history is full";
    out.plan_state = plan.state;
    return out;
  }
  plan.resources = resources;
  plan.policy_generation = policy_generation;
  plan.epoch = epoch_;
  plan.revalidation_required = false;
  plan.revalidation_notes.clear();
  plan.updated_at_tick = now_tick;
  plan.last_transition_tick = now_tick;
  plan.attempts_in_stage = 0;
  plan.cooldown_until_tick = 0;
  plan.last_reason = RejectReason::NONE;
  plan.last_reason_text.clear();
  if (!plan.generation.bump()) {
    plan.state = RecoveryState::FAILED;
    out.reason = RejectReason::LIMIT_EXCEEDED;
    out.reason_text = "plan generation exhausted";
    out.plan_state = plan.state;
    return out;
  }
  if (!plan.stage_generation.bump()) {
    plan.state = RecoveryState::FAILED;
    out.reason = RejectReason::LIMIT_EXCEEDED;
    out.reason_text = "stage generation exhausted";
    out.plan_state = plan.state;
    return out;
  }
  plan.attempt = AttemptId(plan.total_attempts + 1u);
  plan.total_attempts += 1;
  plan.state = state_for_stage(stage->kind);
  if (plan.state == RecoveryState::UNKNOWN) {
    plan.state = RecoveryState::PLANNED;
  }
  {
    StageRecord& record = open_stage_record(plan, *stage, now_tick);
    record.requested = stage->target;
    record.epoch = plan.epoch;
  }
  out.ok = true;
  out.reason = RejectReason::NONE;
  out.reason_text = "plan revalidated";
  out.plan_state = plan.state;
  emit_locked("PLAN_REVALIDATED", plan, out.reason_text);
  return out;
}

// ---------------------------------------------------------------------------
// Explanation
// ---------------------------------------------------------------------------
bool RecoveryEngine::explain(RecoveryPlanId id, RecoveryExplanation& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const PlanSlot* slot = plan_slot_locked(id);
  if (slot == nullptr) {
    return false;
  }
  const RecoveryPlan& plan = slot->plan;
  const PolicySlot* policy_slot = policy_slot_locked(plan.policy_id);
  if (policy_slot == nullptr) {
    return false;
  }
  const RecoveryPolicy& pol = policy_slot->policy;
  const StageSpec* stage = stage_at(pol, plan.stage_index);
  if (stage == nullptr) {
    return false;
  }

  out = RecoveryExplanation{};
  out.plan = plan.id;
  out.plan_generation = plan.generation;
  out.epoch = plan.epoch;
  out.policy_id = plan.policy_id;
  out.policy_generation = plan.policy_generation;
  out.state = plan.state;
  out.stage_kind = stage->kind;
  out.stage_index = plan.stage_index;
  out.stage_count = pol.stages.size();
  out.stage_name = stage->name;
  out.stage_generation = plan.stage_generation;
  out.attempt = plan.attempt;
  out.stale_reason = plan.revalidation_required ? plan.last_reason : RejectReason::NONE;
  out.pause_reason = plan.state == RecoveryState::PAUSED ? plan.last_reason : RejectReason::NONE;
  out.reason_text = plan.last_reason_text;
  out.attempts_in_stage = plan.attempts_in_stage;
  out.total_attempts = plan.total_attempts;
  out.rollback_count = plan.rollback_count;
  out.completion_count = plan.completion_count;
  out.revalidation_required = plan.revalidation_required;
  out.notes = plan.revalidation_notes;

  const std::uint64_t elapsed = plan.updated_at_tick >= plan.last_transition_tick
                                    ? plan.updated_at_tick - plan.last_transition_tick
                                    : 0u;
  out.dwell_remaining_ticks = elapsed >= stage->dwell_ticks ? 0u : stage->dwell_ticks - elapsed;
  out.cooldown_remaining_ticks =
      plan.updated_at_tick >= plan.cooldown_until_tick ? 0u : plan.cooldown_until_tick - plan.updated_at_tick;

  const std::size_t item_limit = static_cast<std::size_t>(limits_.max_explanation_items);
  for (const EvidenceRequirement& req : stage->requirements) {
    if (out.required_evidence.size() >= item_limit) {
      break;
    }
    EvidenceEvaluation ev{};
    ev.kind = req.kind;
    std::string text;
    text += to_string(req.kind);
    text += " requires samples>=";
    text += std::to_string(req.min_samples);
    text += " provenance>=";
    text += to_string(req.min_provenance);
    text += " confidence>=";
    text += std::to_string(req.min_confidence);
    if (req.has_min_value) {
      text += " value>=";
      text += std::to_string(req.min_value);
    }
    if (req.has_max_value) {
      text += " value<=";
      text += std::to_string(req.max_value);
    }
    if (req.require_stable) {
      text += " stable";
    }
    if (req.require_affirmative) {
      text += " affirmative";
    }
    ev.detail = bound_text(text, limits_.max_reason_length);
    out.required_evidence.push_back(ev);
  }

  for (const RestorationAmount& amount : stage->target.entries()) {
    if (out.restoration.size() >= item_limit) {
      break;
    }
    RestorationView view{};
    view.resource = amount.resource;
    view.unit = amount.unit;
    const RestorationAmount* base = plan.baseline.find(amount.resource);
    view.baseline = base != nullptr ? base->effective : 0u;
    view.baseline_known = plan.baseline_known && base != nullptr;
    const RestorationAmount* constrained = plan.constrained.find(amount.resource);
    view.constrained = constrained != nullptr ? constrained->effective : 0u;
    view.requested = amount.requested;
    const RestorationAmount* authorized = plan.authorized.find(amount.resource);
    view.authorized = authorized != nullptr ? authorized->effective : 0u;
    view.effective = amount.effective;
    const RestorationAmount* observed = plan.last_observed.find(amount.resource);
    view.observed = observed != nullptr ? observed->observed : 0u;
    view.observed_known = observed != nullptr && observed->observed_known;
    const RestorationAmount* target = plan.rollback_target.find(amount.resource);
    view.rollback_target = target != nullptr ? target->effective : 0u;
    view.authority_clamped = amount.authority_clamped;
    out.restoration.push_back(view);
  }

  for (const ProtectedObligation& obligation : plan.obligations) {
    if (out.obligations.size() >= item_limit) {
      break;
    }
    out.obligations.push_back(evaluate_obligation(obligation, stage->target));
  }
  for (const ResourceBinding& binding : plan.resources) {
    if (out.authority.size() >= item_limit) {
      break;
    }
    AuthorityView view{};
    view.resource = binding.resource;
    const RecoveryAuthority* grant = authority_.find(binding.resource);
    if (grant != nullptr) {
      view.present = true;
      view.unit = grant->unit;
      view.generation = grant->generation;
      view.ceiling = grant->ceiling;
      view.floor = grant->floor;
      view.epoch_match = grant->epoch == epoch_;
      view.expired = grant->expires_at_tick <= plan.updated_at_tick;
    }
    out.authority.push_back(view);
  }
  for (const RollbackRecord& record : plan.rollback_history) {
    if (out.notes.size() >= item_limit) {
      break;
    }
    std::string note = "rollback ";
    note += std::to_string(record.id.value());
    note += " kind=";
    note += to_string(record.kind);
    note += " compensation_steps=";
    note += std::to_string(record.compensation_steps);
    note += " unobserved=";
    note += std::to_string(record.unobserved.size());
    out.notes.push_back(bound_text(note, limits_.max_reason_length));
  }
  return true;
}

std::string RecoveryEngine::render_explanation(RecoveryPlanId id) const {
  RecoveryExplanation explanation{};
  if (!explain(id, explanation)) {
    return "explanation unavailable\n";
  }
  return explanation.render();
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------
DurableState RecoveryEngine::export_state() const {
  std::lock_guard<std::mutex> guard(mutex_);
  DurableState state{};
  state.format_version = kSnapshotFormatVersion;
  state.epoch = epoch_;
  state.policies.reserve(policies_.size());
  for (const auto& entry : policies_) {
    state.policies.push_back(entry.second->policy);
  }
  std::sort(state.policies.begin(), state.policies.end(),
            [](const RecoveryPolicy& a, const RecoveryPolicy& b) { return a.id < b.id; });
  state.plans.reserve(plans_.size());
  for (const auto& entry : plans_) {
    state.plans.push_back(entry.second->plan);
  }
  std::sort(state.plans.begin(), state.plans.end(),
            [](const RecoveryPlan& a, const RecoveryPlan& b) { return a.id < b.id; });
  for (const RecoveryPlan& plan : state.plans) {
    state.snapshot_tick = max_u64(state.snapshot_tick, plan.updated_at_tick);
  }
  return state;
}

RestoreReport RecoveryEngine::import_state(const DurableState& state, CoordinatorEpoch new_epoch) {
  std::lock_guard<std::mutex> guard(mutex_);
  RestoreReport report{};
  report.epoch_before = state.epoch.value();
  report.epoch_after = new_epoch.value();
  if (state.format_version != kSnapshotFormatVersion) {
    report.valid = false;
    report.detail = "unsupported durable state version";
    return report;
  }
  if (!new_epoch.is_valid() || new_epoch <= state.epoch) {
    report.valid = false;
    report.detail = "coordinator epoch must advance on restart";
    return report;
  }
  if (state.policies.size() > static_cast<std::size_t>(limits_.max_policies) ||
      state.plans.size() > static_cast<std::size_t>(limits_.max_plans)) {
    report.valid = false;
    report.detail = "durable state exceeds configured limits";
    return report;
  }
  for (const RecoveryPolicy& policy : state.policies) {
    std::string err;
    if (!validate_policy(policy, limits_, err)) {
      report.valid = false;
      report.detail = "durable policy failed validation: " + err;
      return report;
    }
  }

  policies_.clear();
  plans_.clear();
  authority_.clear();
  lineage_.clear();
  next_policy_id_ = 1;
  next_plan_id_ = 1;

  for (const RecoveryPolicy& policy : state.policies) {
    auto slot = std::make_unique<PolicySlot>();
    slot->policy = policy;
    policies_.emplace(policy.id.value(), std::move(slot));
    if (policy.id.value() >= next_policy_id_ && policy.id.value() < kU64Max) {
      next_policy_id_ = policy.id.value() + 1u;
    }
    report.policies_loaded += 1;
  }

  for (const RecoveryPlan& loaded : state.plans) {
    if (policies_.find(loaded.policy_id.value()) == policies_.end()) {
      report.valid = false;
      report.loaded = false;
      report.detail = "durable plan references an absent policy";
      policies_.clear();
      plans_.clear();
      return report;
    }
    auto slot = std::make_unique<PlanSlot>();
    RecoveryPlan& plan = slot->plan;
    plan = loaded;
    // Liveness, live authority and telemetry freshness never come back from
    // disk. A plan that was mid-recovery must revalidate before advancing.
    if (is_live(plan.state)) {
      plan.revalidation_required = true;
      plan.state = RecoveryState::REVALIDATION_REQUIRED;
      plan.last_reason = RejectReason::STALE_EVIDENCE;
      plan.last_reason_text = "restart: live evidence and authority are not durable";
      plan.revalidation_notes.clear();
      if (plan.revalidation_notes.size() < static_cast<std::size_t>(limits_.max_explanation_items)) {
        plan.revalidation_notes.push_back("restart requires revalidation");
      }
      report.plans_requiring_revalidation += 1;
      report.revalidation_plans.push_back(plan.id);
      if (!plan.authorized.empty()) {
        report.authorities_dropped += 1;
      }
    }
    plan.epoch = new_epoch;
    plan.last_observed.clear();
    for (const CompletionRecord& record : plan.completions) {
      if (record.state == CompletionState::COMMITTED) {
        report.completions_preserved += 1;
      } else if (record.state == CompletionState::FAILED) {
        report.completions_failed_preserved += 1;
      }
    }
    report.rollbacks_preserved += static_cast<std::uint32_t>(plan.rollback_history.size());
    report.plans_loaded += 1;
    if (plan.id.value() >= next_plan_id_ && plan.id.value() < kU64Max) {
      next_plan_id_ = plan.id.value() + 1u;
    }
    plans_.emplace(plan.id.value(), std::move(slot));
  }

  epoch_ = new_epoch;
  report.loaded = true;
  report.valid = true;
  report.detail = "durable state restored; live authority and liveness were not";
  return report;
}

std::vector<LineageEvent> RecoveryEngine::drain_lineage_events() {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<LineageEvent> out = std::move(lineage_);
  lineage_.clear();
  return out;
}

void RecoveryEngine::emit_locked(const std::string& kind, const RecoveryPlan& plan, const std::string& detail) {
  if (lineage_.size() >= static_cast<std::size_t>(limits_.max_adjacent_requests)) {
    lineage_.erase(lineage_.begin());
  }
  LineageEvent event{};
  event.kind = kind;
  event.plan = plan.id;
  event.plan_generation = plan.generation;
  event.epoch = epoch_;
  event.tick = plan.updated_at_tick;
  event.detail = bound_text(detail, limits_.max_reason_length);
  lineage_.push_back(event);
}

bool RecoveryEngine::plan_obligations_hold(const RecoveryPlan& plan, const RestorationVector& levels,
                                           std::string& detail) const {
  for (const ProtectedObligation& obligation : plan.obligations) {
    const ObligationEvaluation evaluation = evaluate_obligation(obligation, levels);
    if (evaluation.blocking) {
      detail = bound_text(evaluation.detail, limits_.max_reason_length);
      return false;
    }
  }
  return true;
}

std::uint64_t RecoveryEngine::evaluations() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluations_;
}

std::uint64_t RecoveryEngine::advances() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return advances_;
}

std::uint64_t RecoveryEngine::stale_rejections() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stale_rejections_;
}

std::uint64_t RecoveryEngine::completion_replays() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return completion_replays_;
}

std::uint64_t RecoveryEngine::rollbacks_performed() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return rollbacks_performed_;
}

const AdjacentRequestLog& RecoveryEngine::adjacent_requests() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return adjacent_;
}

void RecoveryEngine::clear_adjacent_requests() {
  std::lock_guard<std::mutex> guard(mutex_);
  adjacent_.clear();
}

}  // namespace congestion_recovery
