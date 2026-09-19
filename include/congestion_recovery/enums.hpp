// Congestion Recovery - lifecycle, decision and evidence enumerations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>

namespace congestion_recovery {

// ---------------------------------------------------------------------------
// Stage ladder. OBSERVE -> PROBE -> PARTIAL_RESTORE -> HOLD -> EXPAND -> COMPLETE
// ---------------------------------------------------------------------------
enum class StageKind : std::uint8_t {
  UNKNOWN = 0,
  OBSERVE = 1,
  PROBE = 2,
  PARTIAL_RESTORE = 3,
  HOLD = 4,
  EXPAND = 5,
  COMPLETE = 6,
  COUNT_ = 7,
};

[[nodiscard]] constexpr bool is_valid(StageKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(StageKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(StageKind v) noexcept {
  switch (v) {
    case StageKind::OBSERVE: return "OBSERVE";
    case StageKind::PROBE: return "PROBE";
    case StageKind::PARTIAL_RESTORE: return "PARTIAL_RESTORE";
    case StageKind::HOLD: return "HOLD";
    case StageKind::EXPAND: return "EXPAND";
    case StageKind::COMPLETE: return "COMPLETE";
    case StageKind::UNKNOWN:
    case StageKind::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Plan lifecycle state.
// ---------------------------------------------------------------------------
enum class RecoveryState : std::uint8_t {
  UNKNOWN = 0,
  PLANNED = 1,
  OBSERVING = 2,
  PROBING = 3,
  PARTIAL_RESTORE = 4,
  HOLDING = 5,
  EXPANDING = 6,
  COMPLETED = 7,
  PAUSED = 8,
  ROLLING_BACK = 9,
  ROLLED_BACK = 10,
  STALE = 11,
  REVALIDATION_REQUIRED = 12,
  FAILED = 13,
  ABORTED = 14,
  // Occupying the terminal COMPLETE stage while completion evidence is still
  // being verified. This is deliberately NOT "completed": entering the stage
  // pro ves nothing until the applied restoration has been observed.
  VERIFYING = 15,
  COUNT_ = 16,
};

[[nodiscard]] constexpr bool is_valid(RecoveryState v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RecoveryState::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RecoveryState v) noexcept {
  switch (v) {
    case RecoveryState::PLANNED: return "PLANNED";
    case RecoveryState::OBSERVING: return "OBSERVING";
    case RecoveryState::PROBING: return "PROBING";
    case RecoveryState::PARTIAL_RESTORE: return "PARTIAL_RESTORE";
    case RecoveryState::HOLDING: return "HOLDING";
    case RecoveryState::EXPANDING: return "EXPANDING";
    case RecoveryState::COMPLETED: return "COMPLETED";
    case RecoveryState::PAUSED: return "PAUSED";
    case RecoveryState::ROLLING_BACK: return "ROLLING_BACK";
    case RecoveryState::ROLLED_BACK: return "ROLLED_BACK";
    case RecoveryState::STALE: return "STALE";
    case RecoveryState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case RecoveryState::FAILED: return "FAILED";
    case RecoveryState::ABORTED: return "ABORTED";
    case RecoveryState::VERIFYING: return "VERIFYING";
    case RecoveryState::UNKNOWN:
    case RecoveryState::COUNT_: break;
  }
  return "UNKNOWN";
}

// True when the plan may still change authoritative state.
[[nodiscard]] constexpr bool is_live(RecoveryState v) noexcept {
  switch (v) {
    case RecoveryState::PLANNED:
    case RecoveryState::OBSERVING:
    case RecoveryState::PROBING:
    case RecoveryState::PARTIAL_RESTORE:
    case RecoveryState::HOLDING:
    case RecoveryState::EXPANDING:
    case RecoveryState::PAUSED:
    case RecoveryState::ROLLING_BACK:
    case RecoveryState::ROLLED_BACK:
    case RecoveryState::REVALIDATION_REQUIRED:
    case RecoveryState::VERIFYING:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr RecoveryState state_for_stage(StageKind k) noexcept {
  switch (k) {
    case StageKind::OBSERVE: return RecoveryState::OBSERVING;
    case StageKind::PROBE: return RecoveryState::PROBING;
    case StageKind::PARTIAL_RESTORE: return RecoveryState::PARTIAL_RESTORE;
    case StageKind::HOLD: return RecoveryState::HOLDING;
    case StageKind::EXPAND: return RecoveryState::EXPANDING;
    case StageKind::COMPLETE: return RecoveryState::VERIFYING;
    default: return RecoveryState::UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Decision taken by a deterministic transition evaluation.
// ---------------------------------------------------------------------------
enum class RecoveryDecision : std::uint8_t {
  NONE = 0,
  HOLD = 1,
  ADVANCE_STAGE = 2,
  COMPLETE = 3,
  PAUSE = 4,
  RESUME = 5,
  ROLLBACK = 6,
  REJECT_STALE = 7,
  REJECT_AUTHORITY = 8,
  REJECT_OBLIGATION = 9,
  REJECT_EVIDENCE = 10,
  REVALIDATE = 11,
  FAIL = 12,
  NO_OP = 13,
  COUNT_ = 14,
};

[[nodiscard]] constexpr bool is_valid(RecoveryDecision v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RecoveryDecision::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RecoveryDecision v) noexcept {
  switch (v) {
    case RecoveryDecision::HOLD: return "HOLD";
    case RecoveryDecision::ADVANCE_STAGE: return "ADVANCE_STAGE";
    case RecoveryDecision::COMPLETE: return "COMPLETE";
    case RecoveryDecision::PAUSE: return "PAUSE";
    case RecoveryDecision::RESUME: return "RESUME";
    case RecoveryDecision::ROLLBACK: return "ROLLBACK";
    case RecoveryDecision::REJECT_STALE: return "REJECT_STALE";
    case RecoveryDecision::REJECT_AUTHORITY: return "REJECT_AUTHORITY";
    case RecoveryDecision::REJECT_OBLIGATION: return "REJECT_OBLIGATION";
    case RecoveryDecision::REJECT_EVIDENCE: return "REJECT_EVIDENCE";
    case RecoveryDecision::REVALIDATE: return "REVALIDATE";
    case RecoveryDecision::FAIL: return "FAIL";
    case RecoveryDecision::NO_OP: return "NO_OP";
    case RecoveryDecision::NONE:
    case RecoveryDecision::COUNT_: break;
  }
  return "NONE";
}

// ---------------------------------------------------------------------------
// Rejection / hold reasons. Every non-advance carries exactly one.
// ---------------------------------------------------------------------------
enum class RejectReason : std::uint8_t {
  NONE = 0,
  STALE_EVIDENCE = 1,
  STALE_PLAN = 2,
  STALE_POLICY = 3,
  STALE_EPOCH = 4,
  STALE_RESOURCE = 5,
  STALE_COMPLETION = 6,
  GENERATION_MISMATCH = 7,
  AUTHORITY_INSUFFICIENT = 8,
  AUTHORITY_MISSING = 9,
  AUTHORITY_EXPIRED = 10,
  OBLIGATION_VIOLATION = 11,
  EVIDENCE_INSUFFICIENT = 12,
  EVIDENCE_CONTRADICTORY = 13,
  HYSTERESIS_BAND = 14,
  DWELL_NOT_SATISFIED = 15,
  COOLDOWN_ACTIVE = 16,
  ATTEMPT_BUDGET_EXHAUSTED = 17,
  ROLLBACK_BUDGET_EXHAUSTED = 18,
  TRANSITION_RATE_EXCEEDED = 19,
  RECURRENCE_DETECTED = 20,
  PARTIAL_APPLICATION = 21,
  CAPACITY_DROP = 22,
  COMPLETION_FAILED = 23,
  PLAN_LIFETIME_EXCEEDED = 24,
  REVALIDATION_BOUNDARY = 25,
  INVALID_INPUT = 26,
  LIMIT_EXCEEDED = 27,
  NOT_FOUND = 28,
  NOT_LIVE = 29,
  NO_POLICY_STAGE = 30,
  UNKNOWN = 31,
  COUNT_ = 32,
};

[[nodiscard]] constexpr bool is_valid(RejectReason v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RejectReason::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason v) noexcept {
  switch (v) {
    case RejectReason::NONE: return "NONE";
    case RejectReason::STALE_EVIDENCE: return "STALE_EVIDENCE";
    case RejectReason::STALE_PLAN: return "STALE_PLAN";
    case RejectReason::STALE_POLICY: return "STALE_POLICY";
    case RejectReason::STALE_EPOCH: return "STALE_EPOCH";
    case RejectReason::STALE_RESOURCE: return "STALE_RESOURCE";
    case RejectReason::STALE_COMPLETION: return "STALE_COMPLETION";
    case RejectReason::GENERATION_MISMATCH: return "GENERATION_MISMATCH";
    case RejectReason::AUTHORITY_INSUFFICIENT: return "AUTHORITY_INSUFFICIENT";
    case RejectReason::AUTHORITY_MISSING: return "AUTHORITY_MISSING";
    case RejectReason::AUTHORITY_EXPIRED: return "AUTHORITY_EXPIRED";
    case RejectReason::OBLIGATION_VIOLATION: return "OBLIGATION_VIOLATION";
    case RejectReason::EVIDENCE_INSUFFICIENT: return "EVIDENCE_INSUFFICIENT";
    case RejectReason::EVIDENCE_CONTRADICTORY: return "EVIDENCE_CONTRADICTORY";
    case RejectReason::HYSTERESIS_BAND: return "HYSTERESIS_BAND";
    case RejectReason::DWELL_NOT_SATISFIED: return "DWELL_NOT_SATISFIED";
    case RejectReason::COOLDOWN_ACTIVE: return "COOLDOWN_ACTIVE";
    case RejectReason::ATTEMPT_BUDGET_EXHAUSTED: return "ATTEMPT_BUDGET_EXHAUSTED";
    case RejectReason::ROLLBACK_BUDGET_EXHAUSTED: return "ROLLBACK_BUDGET_EXHAUSTED";
    case RejectReason::TRANSITION_RATE_EXCEEDED: return "TRANSITION_RATE_EXCEEDED";
    case RejectReason::RECURRENCE_DETECTED: return "RECURRENCE_DETECTED";
    case RejectReason::PARTIAL_APPLICATION: return "PARTIAL_APPLICATION";
    case RejectReason::CAPACITY_DROP: return "CAPACITY_DROP";
    case RejectReason::COMPLETION_FAILED: return "COMPLETION_FAILED";
    case RejectReason::PLAN_LIFETIME_EXCEEDED: return "PLAN_LIFETIME_EXCEEDED";
    case RejectReason::REVALIDATION_BOUNDARY: return "REVALIDATION_BOUNDARY";
    case RejectReason::INVALID_INPUT: return "INVALID_INPUT";
    case RejectReason::LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
    case RejectReason::NOT_FOUND: return "NOT_FOUND";
    case RejectReason::NOT_LIVE: return "NOT_LIVE";
    case RejectReason::NO_POLICY_STAGE: return "NO_POLICY_STAGE";
    case RejectReason::UNKNOWN:
    case RejectReason::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Evidence.
// ---------------------------------------------------------------------------
enum class EvidenceKind : std::uint8_t {
  UNKNOWN = 0,
  CONGESTION_CLEARED = 1,
  PATH_STABLE = 2,
  LATENCY_STABLE = 3,
  LOSS_STABLE = 4,
  QUEUE_DEPTH = 5,
  SERVICE_RATE = 6,
  CAPACITY_AVAILABLE = 7,
  APPLIED_RESTORATION = 8,
  INTERVENTION_INACTIVE = 9,
  HEALTH_PROBE_OK = 10,
  PROTECTED_OBLIGATION_STATUS = 11,
  POLICY_CURRENT = 12,
  PEER_CONFIRMATION = 13,
  CUSTOM = 14,
  COUNT_ = 15,
};

[[nodiscard]] constexpr bool is_valid(EvidenceKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(EvidenceKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(EvidenceKind v) noexcept {
  switch (v) {
    case EvidenceKind::CONGESTION_CLEARED: return "CONGESTION_CLEARED";
    case EvidenceKind::PATH_STABLE: return "PATH_STABLE";
    case EvidenceKind::LATENCY_STABLE: return "LATENCY_STABLE";
    case EvidenceKind::LOSS_STABLE: return "LOSS_STABLE";
    case EvidenceKind::QUEUE_DEPTH: return "QUEUE_DEPTH";
    case EvidenceKind::SERVICE_RATE: return "SERVICE_RATE";
    case EvidenceKind::CAPACITY_AVAILABLE: return "CAPACITY_AVAILABLE";
    case EvidenceKind::APPLIED_RESTORATION: return "APPLIED_RESTORATION";
    case EvidenceKind::INTERVENTION_INACTIVE: return "INTERVENTION_INACTIVE";
    case EvidenceKind::HEALTH_PROBE_OK: return "HEALTH_PROBE_OK";
    case EvidenceKind::PROTECTED_OBLIGATION_STATUS: return "PROTECTED_OBLIGATION_STATUS";
    case EvidenceKind::POLICY_CURRENT: return "POLICY_CURRENT";
    case EvidenceKind::PEER_CONFIRMATION: return "PEER_CONFIRMATION";
    case EvidenceKind::CUSTOM: return "CUSTOM";
    case EvidenceKind::UNKNOWN:
    case EvidenceKind::COUNT_: break;
  }
  return "UNKNOWN";
}

// Provenance is ordered by trust. UNKNOWN never satisfies a requirement.
enum class Provenance : std::uint8_t {
  UNKNOWN = 0,
  SYNTHETIC = 1,
  RECONSTRUCTED = 2,
  ESTIMATED = 3,
  DERIVED = 4,
  REPORTED = 5,
  MEASURED = 6,
  COUNT_ = 7,
};

[[nodiscard]] constexpr bool is_valid(Provenance v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(Provenance::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(Provenance v) noexcept {
  switch (v) {
    case Provenance::SYNTHETIC: return "SYNTHETIC";
    case Provenance::RECONSTRUCTED: return "RECONSTRUCTED";
    case Provenance::ESTIMATED: return "ESTIMATED";
    case Provenance::DERIVED: return "DERIVED";
    case Provenance::REPORTED: return "REPORTED";
    case Provenance::MEASURED: return "MEASURED";
    case Provenance::UNKNOWN:
    case Provenance::COUNT_: break;
  }
  return "UNKNOWN";
}

[[nodiscard]] constexpr std::uint8_t provenance_rank(Provenance v) noexcept {
  return static_cast<std::uint8_t>(v);
}

// A UNKNOWN requirement means "no provenance requirement". Otherwise the
// observed provenance must be at least as trustworthy as required, and UNKNOWN
// provenance (rank 0) never satisfies a real requirement.
[[nodiscard]] constexpr bool provenance_at_least(Provenance have, Provenance required) noexcept {
  if (required == Provenance::UNKNOWN) {
    return true;
  }
  return provenance_rank(have) >= provenance_rank(required) && have != Provenance::UNKNOWN;
}

// ---------------------------------------------------------------------------
// Restoration units.
// ---------------------------------------------------------------------------
enum class RestorationUnit : std::uint8_t {
  UNKNOWN = 0,
  BYTES_PER_SECOND = 1,
  FLOWS = 2,
  WEIGHT_UNITS = 3,
  PERCENT_BASIS_POINTS = 4,
  QUEUE_DEPTH_UNITS = 5,
  CONCURRENCY_SLOTS = 6,
  COUNT_ = 7,
};

[[nodiscard]] constexpr bool is_valid(RestorationUnit v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RestorationUnit::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RestorationUnit v) noexcept {
  switch (v) {
    case RestorationUnit::BYTES_PER_SECOND: return "BYTES_PER_SECOND";
    case RestorationUnit::FLOWS: return "FLOWS";
    case RestorationUnit::WEIGHT_UNITS: return "WEIGHT_UNITS";
    case RestorationUnit::PERCENT_BASIS_POINTS: return "PERCENT_BASIS_POINTS";
    case RestorationUnit::QUEUE_DEPTH_UNITS: return "QUEUE_DEPTH_UNITS";
    case RestorationUnit::CONCURRENCY_SLOTS: return "CONCURRENCY_SLOTS";
    case RestorationUnit::UNKNOWN:
    case RestorationUnit::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Protected obligations.
// ---------------------------------------------------------------------------
enum class ObligationKind : std::uint8_t {
  UNKNOWN = 0,
  MIN_SERVICE_RATE = 1,
  RESERVED_CAPACITY = 2,
  PROTECTED_CLASS_FLOOR = 3,
  PRIORITY_FLOOR = 4,
  MAX_QUEUE_DEPTH = 5,
  MAX_LATENCY = 6,
  CUSTOM = 7,
  COUNT_ = 8,
};

[[nodiscard]] constexpr bool is_valid(ObligationKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(ObligationKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(ObligationKind v) noexcept {
  switch (v) {
    case ObligationKind::MIN_SERVICE_RATE: return "MIN_SERVICE_RATE";
    case ObligationKind::RESERVED_CAPACITY: return "RESERVED_CAPACITY";
    case ObligationKind::PROTECTED_CLASS_FLOOR: return "PROTECTED_CLASS_FLOOR";
    case ObligationKind::PRIORITY_FLOOR: return "PRIORITY_FLOOR";
    case ObligationKind::MAX_QUEUE_DEPTH: return "MAX_QUEUE_DEPTH";
    case ObligationKind::MAX_LATENCY: return "MAX_LATENCY";
    case ObligationKind::CUSTOM: return "CUSTOM";
    case ObligationKind::UNKNOWN:
    case ObligationKind::COUNT_: break;
  }
  return "UNKNOWN";
}

// A floor obligation requires value >= threshold; a ceiling requires <=.
[[nodiscard]] constexpr bool is_floor_obligation(ObligationKind v) noexcept {
  switch (v) {
    case ObligationKind::MIN_SERVICE_RATE:
    case ObligationKind::RESERVED_CAPACITY:
    case ObligationKind::PROTECTED_CLASS_FLOOR:
    case ObligationKind::PRIORITY_FLOOR:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr bool is_ceiling_obligation(ObligationKind v) noexcept {
  switch (v) {
    case ObligationKind::MAX_QUEUE_DEPTH:
    case ObligationKind::MAX_LATENCY:
      return true;
    default:
      return false;
  }
}

enum class ObligationState : std::uint8_t {
  UNKNOWN = 0,
  SATISFIED = 1,
  VIOLATED = 2,
  NOT_EVALUATED = 3,
  COUNT_ = 4,
};

[[nodiscard]] constexpr bool is_valid(ObligationState v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(ObligationState::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(ObligationState v) noexcept {
  switch (v) {
    case ObligationState::SATISFIED: return "SATISFIED";
    case ObligationState::VIOLATED: return "VIOLATED";
    case ObligationState::NOT_EVALUATED: return "NOT_EVALUATED";
    case ObligationState::UNKNOWN:
    case ObligationState::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Stage / rollback / completion records.
// ---------------------------------------------------------------------------
enum class StageOutcome : std::uint8_t {
  UNKNOWN = 0,
  PENDING = 1,
  SUCCEEDED = 2,
  PARTIAL = 3,
  FAILED = 4,
  STALE = 5,
  REJECTED = 6,
  ROLLED_BACK = 7,
  COUNT_ = 8,
};

[[nodiscard]] constexpr bool is_valid(StageOutcome v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(StageOutcome::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(StageOutcome v) noexcept {
  switch (v) {
    case StageOutcome::PENDING: return "PENDING";
    case StageOutcome::SUCCEEDED: return "SUCCEEDED";
    case StageOutcome::PARTIAL: return "PARTIAL";
    case StageOutcome::FAILED: return "FAILED";
    case StageOutcome::STALE: return "STALE";
    case StageOutcome::REJECTED: return "REJECTED";
    case StageOutcome::ROLLED_BACK: return "ROLLED_BACK";
    case StageOutcome::UNKNOWN:
    case StageOutcome::COUNT_: break;
  }
  return "UNKNOWN";
}

enum class RollbackKind : std::uint8_t {
  UNKNOWN = 0,
  COMPENSATING = 1,
  RESTORE_BASELINE = 2,
  HOLD_SAFE = 3,
  PARTIAL_COMPENSATION = 4,
  COUNT_ = 5,
};

[[nodiscard]] constexpr bool is_valid(RollbackKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u && static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RollbackKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RollbackKind v) noexcept {
  switch (v) {
    case RollbackKind::COMPENSATING: return "COMPENSATING";
    case RollbackKind::RESTORE_BASELINE: return "RESTORE_BASELINE";
    case RollbackKind::HOLD_SAFE: return "HOLD_SAFE";
    case RollbackKind::PARTIAL_COMPENSATION: return "PARTIAL_COMPENSATION";
    case RollbackKind::UNKNOWN:
    case RollbackKind::COUNT_: break;
  }
  return "UNKNOWN";
}

enum class CompletionState : std::uint8_t {
  UNKNOWN = 0,
  PENDING = 1,
  COMMITTED = 2,
  FAILED = 3,
  REJECTED_STALE = 4,
  COUNT_ = 5,
};

[[nodiscard]] constexpr bool is_valid(CompletionState v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(CompletionState::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(CompletionState v) noexcept {
  switch (v) {
    case CompletionState::PENDING: return "PENDING";
    case CompletionState::COMMITTED: return "COMMITTED";
    case CompletionState::FAILED: return "FAILED";
    case CompletionState::REJECTED_STALE: return "REJECTED_STALE";
    case CompletionState::UNKNOWN:
    case CompletionState::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Recurrence handling policy.
// ---------------------------------------------------------------------------
enum class RecurrenceAction : std::uint8_t {
  UNKNOWN = 0,
  PAUSE = 1,
  ROLLBACK = 2,
  FAIL = 3,
  COUNT_ = 4,
};

[[nodiscard]] constexpr bool is_valid(RecurrenceAction v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(RecurrenceAction::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(RecurrenceAction v) noexcept {
  switch (v) {
    case RecurrenceAction::PAUSE: return "PAUSE";
    case RecurrenceAction::ROLLBACK: return "ROLLBACK";
    case RecurrenceAction::FAIL: return "FAIL";
    case RecurrenceAction::UNKNOWN:
    case RecurrenceAction::COUNT_: break;
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Adjacent-runtime request kinds. Congestion Recovery never performs these
// actions itself; it emits bounded, generation-bound requests.
// ---------------------------------------------------------------------------
enum class AdjacentRequestKind : std::uint8_t {
  UNKNOWN = 0,
  REQUEST_PATH_REVALIDATION = 1,
  REQUEST_PACING_CHANGE = 2,
  REQUEST_ADMISSION_RELIEF = 3,
  REQUEST_CONGESTION_OBSERVATION = 4,
  REQUEST_TRAFFIC_ENGINEERING_REVIEW = 5,
  REQUEST_INTERVENTION_RELEASE = 6,
  NOTIFY_RECOVERY_COMPLETE = 7,
  NOTIFY_RECOVERY_ROLLBACK = 8,
  COUNT_ = 9,
};

[[nodiscard]] constexpr bool is_valid(AdjacentRequestKind v) noexcept {
  return static_cast<std::uint8_t>(v) > 0u &&
         static_cast<std::uint8_t>(v) < static_cast<std::uint8_t>(AdjacentRequestKind::COUNT_);
}

[[nodiscard]] constexpr std::string_view to_string(AdjacentRequestKind v) noexcept {
  switch (v) {
    case AdjacentRequestKind::REQUEST_PATH_REVALIDATION: return "REQUEST_PATH_REVALIDATION";
    case AdjacentRequestKind::REQUEST_PACING_CHANGE: return "REQUEST_PACING_CHANGE";
    case AdjacentRequestKind::REQUEST_ADMISSION_RELIEF: return "REQUEST_ADMISSION_RELIEF";
    case AdjacentRequestKind::REQUEST_CONGESTION_OBSERVATION: return "REQUEST_CONGESTION_OBSERVATION";
    case AdjacentRequestKind::REQUEST_TRAFFIC_ENGINEERING_REVIEW: return "REQUEST_TRAFFIC_ENGINEERING_REVIEW";
    case AdjacentRequestKind::REQUEST_INTERVENTION_RELEASE: return "REQUEST_INTERVENTION_RELEASE";
    case AdjacentRequestKind::NOTIFY_RECOVERY_COMPLETE: return "NOTIFY_RECOVERY_COMPLETE";
    case AdjacentRequestKind::NOTIFY_RECOVERY_ROLLBACK: return "NOTIFY_RECOVERY_ROLLBACK";
    case AdjacentRequestKind::UNKNOWN:
    case AdjacentRequestKind::COUNT_: break;
  }
  return "UNKNOWN";
}

}  // namespace congestion_recovery
