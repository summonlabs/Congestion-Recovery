// Congestion Recovery - policy validation.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/policy.hpp"

namespace congestion_recovery {
namespace {

constexpr std::uint64_t kMaxReasonableTicks = 1000000000000ull;

bool set_err(std::string& err, const char* text) {
  if (err.size() < 200u) {
    err = text;
  }
  return false;
}

bool validate_requirement(const EvidenceRequirement& r, std::size_t stage_index, std::string& err) {
  if (!is_valid(r.kind)) {
    return set_err(err, "requirement has invalid evidence kind");
  }
  if (r.min_confidence < 0.0 || r.min_confidence > 1.0) {
    return set_err(err, "requirement confidence outside [0,1]");
  }
  if (r.has_max_value && r.has_min_value && r.max_value < r.min_value) {
    return set_err(err, "requirement max_value below min_value");
  }
  if (r.require_affirmative && r.require_stable && r.stability_window_ticks == 0u) {
    return set_err(err, "requirement demands stable affirmation without a window");
  }
  if (r.min_samples > 1000000u) {
    return set_err(err, "requirement min_samples out of range");
  }
  (void)stage_index;
  return true;
}

}  // namespace

bool EngineLimits::valid() const noexcept {
  if (max_policies == 0u || max_plans == 0u || max_stages_per_policy == 0u || max_resources_per_plan == 0u) {
    return false;
  }
  if (max_obligations_per_plan == 0u || max_evidence_entries == 0u || max_requirements_per_stage == 0u) {
    return false;
  }
  if (max_stage_history == 0u || max_rollback_history == 0u || max_completion_history == 0u) {
    return false;
  }
  if (max_explanation_items == 0u || max_reason_length == 0u || max_adjacent_requests == 0u) {
    return false;
  }
  if (max_attempts_per_stage == 0u || max_total_attempts == 0u || max_rollbacks == 0u) {
    return false;
  }
  if (max_policies > 4096u || max_plans > 65536u || max_stages_per_policy > 64u) {
    return false;
  }
  if (max_resources_per_plan > 4096u || max_obligations_per_plan > 4096u || max_evidence_entries > 8192u) {
    return false;
  }
  if (max_ticks == 0u || max_ticks > kMaxReasonableTicks) {
    return false;
  }
  return true;
}

const StageSpec* stage_at(const RecoveryPolicy& policy, std::size_t index) noexcept {
  return index < policy.stages.size() ? &policy.stages[index] : nullptr;
}

bool validate_policy(const RecoveryPolicy& policy, const EngineLimits& limits, std::string& err) {
  err.clear();
  if (!limits.valid()) {
    return set_err(err, "engine limits invalid");
  }
  if (!policy.id.is_valid()) {
    return set_err(err, "policy id invalid");
  }
  if (!policy.generation.is_valid()) {
    return set_err(err, "policy generation invalid");
  }
  if (policy.stages.empty()) {
    return set_err(err, "policy has no stages");
  }
  if (policy.stages.size() > limits.max_stages_per_policy) {
    return set_err(err, "policy exceeds max stages");
  }
  if (policy.stages.front().kind != StageKind::OBSERVE) {
    return set_err(err, "policy must start with OBSERVE");
  }
  if (policy.stages.back().kind != StageKind::COMPLETE) {
    return set_err(err, "policy must end with COMPLETE");
  }
  if (policy.stages.front().requires_authority) {
    return set_err(err, "OBSERVE stage must not require restoration authority");
  }

  StageKind previous = StageKind::UNKNOWN;
  for (std::size_t i = 0; i < policy.stages.size(); ++i) {
    const StageSpec& s = policy.stages[i];
    if (!is_valid(s.kind)) {
      return set_err(err, "stage has invalid kind");
    }
    if (static_cast<std::uint8_t>(s.kind) <= static_cast<std::uint8_t>(previous)) {
      return set_err(err, "stage kinds must strictly increase");
    }
    previous = s.kind;
    if (!s.target.structurally_valid()) {
      return set_err(err, "stage target vector is not structurally valid");
    }
    if (s.requirements.empty()) {
      return set_err(err, "stage has no evidence requirements");
    }
    if (s.requirements.size() > limits.max_requirements_per_stage) {
      return set_err(err, "stage exceeds max requirements");
    }
    if (s.max_attempts == 0u || s.max_attempts > limits.max_attempts_per_stage) {
      return set_err(err, "stage max_attempts out of range");
    }
    if (s.dwell_ticks > limits.max_ticks || s.stability_window_ticks > limits.max_ticks) {
      return set_err(err, "stage timing out of range");
    }
    std::size_t primaries = 0;
    EvidenceKind previous_kind = EvidenceKind::UNKNOWN;
    for (const EvidenceRequirement& r : s.requirements) {
      if (!validate_requirement(r, i, err)) {
        return false;
      }
      if (r.primary) {
        ++primaries;
      }
      if (static_cast<std::uint8_t>(r.kind) < static_cast<std::uint8_t>(previous_kind)) {
        return set_err(err, "stage requirements must be ordered by kind");
      }
      previous_kind = r.kind;
    }
    if (primaries != 1u) {
      return set_err(err, "stage must have exactly one primary requirement");
    }
    const EvidenceRequirement* primary = nullptr;
    for (const EvidenceRequirement& r : s.requirements) {
      if (r.primary) {
        primary = &r;
      }
    }
    if (primary == nullptr) {
      // Unreachable while the count above is exact, but stated so the primary
      // requirement is never dereferenced on an unproven path.
      return set_err(err, "stage primary requirement is missing");
    }
    if (policy.hysteresis.rollback_margin > 0u) {
      if (!primary->has_min_value) {
        return set_err(err, "hysteresis with rollback margin requires a primary floor");
      }
      if (primary->min_value <= policy.hysteresis.rollback_margin) {
        return set_err(err, "primary floor must exceed rollback margin");
      }
    }
    if (policy.hysteresis.advance_margin > 0u && !primary->has_min_value) {
      return set_err(err, "hysteresis with advance margin requires a primary floor");
    }
    const CheckedU64 ceiling = add_u64(primary->min_value, policy.hysteresis.advance_margin);
    if (ceiling.overflow) {
      return set_err(err, "advance threshold overflows");
    }
    if (!s.requires_authority) {
      const StageSpec* previous_stage = i == 0u ? nullptr : &policy.stages[i - 1u];
      if (previous_stage != nullptr && !s.target.equals(previous_stage->target)) {
        return set_err(err, "stage changes restoration levels but does not require authority");
      }
    }
  }

  if (policy.max_evidence_age_ticks == 0u || policy.max_evidence_age_ticks > limits.max_ticks) {
    return set_err(err, "max_evidence_age_ticks out of range");
  }
  if (policy.max_plan_lifetime_ticks == 0u || policy.max_plan_lifetime_ticks > limits.max_ticks) {
    return set_err(err, "max_plan_lifetime_ticks out of range");
  }
  if (policy.revalidation_boundary_ticks == 0u || policy.revalidation_boundary_ticks > limits.max_ticks) {
    return set_err(err, "revalidation_boundary_ticks out of range");
  }
  if (policy.max_rollbacks > limits.max_rollbacks) {
    return set_err(err, "max_rollbacks exceeds engine limit");
  }
  if (policy.max_total_attempts == 0u || policy.max_total_attempts > limits.max_total_attempts) {
    return set_err(err, "max_total_attempts out of range");
  }
  if (policy.partial_application_tolerance_bp > 10000u) {
    return set_err(err, "partial application tolerance out of range");
  }
  if (!is_valid(policy.on_recurrence)) {
    return set_err(err, "recurrence action invalid");
  }
  if (policy.hysteresis.max_transitions_per_window == 0u ||
      policy.hysteresis.max_transitions_per_window > 1024u) {
    return set_err(err, "max_transitions_per_window out of range");
  }
  if (policy.hysteresis.window_ticks == 0u || policy.hysteresis.window_ticks > limits.max_ticks) {
    return set_err(err, "hysteresis window out of range");
  }
  if (policy.hysteresis.cooldown_ticks > limits.max_ticks) {
    return set_err(err, "cooldown out of range");
  }
  if (policy.name.size() > limits.max_reason_length) {
    return set_err(err, "policy name too long");
  }
  return true;
}

}  // namespace congestion_recovery
