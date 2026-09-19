// Congestion Recovery - protected obligation evaluation.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/obligations.hpp"

namespace congestion_recovery {
namespace {

std::string describe(const ProtectedObligation& o, std::uint64_t candidate, bool satisfied) {
  std::string s;
  s.reserve(96);
  s += to_string(o.kind);
  s += satisfied ? " satisfied" : " violated";
  s += " threshold=";
  s += std::to_string(o.threshold);
  s += " candidate=";
  s += std::to_string(candidate);
  s += o.hard ? " hard" : " soft";
  return s;
}

}  // namespace

ObligationEvaluation evaluate_obligation(const ProtectedObligation& obligation,
                                         const RestorationVector& candidate) noexcept {
  ObligationEvaluation ev{};
  ev.id = obligation.id;
  ev.kind = obligation.kind;
  ev.resource = obligation.resource;
  ev.threshold = obligation.threshold;
  ev.hard = obligation.hard;

  if (!obligation.active) {
    ev.state = ObligationState::NOT_EVALUATED;
    ev.candidate = 0;
    ev.detail = "inactive";
    return ev;
  }

  const RestorationAmount* amount = candidate.find(obligation.resource);
  if (amount == nullptr) {
    // Absence of a candidate level is not evidence of protection.
    ev.state = ObligationState::NOT_EVALUATED;
    ev.candidate = 0;
    ev.blocking = obligation.hard;
    ev.detail = "no candidate level for bound resource";
    return ev;
  }

  ev.candidate = amount->effective;
  bool satisfied = false;
  if (is_floor_obligation(obligation.kind)) {
    satisfied = amount->effective >= obligation.threshold;
  } else if (is_ceiling_obligation(obligation.kind)) {
    satisfied = amount->effective <= obligation.threshold;
  } else {
    // Unknown obligation kinds are never treated as satisfied.
    ev.state = ObligationState::NOT_EVALUATED;
    ev.blocking = obligation.hard;
    ev.detail = "unknown obligation kind";
    return ev;
  }

  ev.state = satisfied ? ObligationState::SATISFIED : ObligationState::VIOLATED;
  ev.blocking = obligation.hard && !satisfied;
  ev.detail = describe(obligation, amount->effective, satisfied);
  return ev;
}

}  // namespace congestion_recovery
