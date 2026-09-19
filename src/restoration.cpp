// Congestion Recovery - restoration vector and compensating-plan computation.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

CompensatingPlan build_compensating_plan(const RestorationVector& observed_current,
                                         const RestorationVector& target) {
  CompensatingPlan plan{};
  plan.complete = true;

  // Only resources present in the rollback target are eligible: the target is
  // the authority on where recovery must land.
  for (const RestorationAmount& t : target.entries()) {
    RestorationDelta step{};
    step.resource = t.resource;
    step.unit = t.unit;

    const RestorationAmount* observed = observed_current.find(t.resource);
    if (observed == nullptr || !observed->observed_known) {
      // Observed state is unknown. Never guess a compensation for it.
      plan.unobserved.push_back(t.resource);
      plan.complete = false;
      continue;
    }
    if (observed->unit != t.unit) {
      plan.unobserved.push_back(t.resource);
      plan.complete = false;
      continue;
    }
    if (observed->observed <= t.effective) {
      plan.already_at_target.push_back(t.resource);
      (void)plan.target.put(t);
      continue;
    }
    const std::uint64_t excess = observed->observed - t.effective;
    if (excess > static_cast<std::uint64_t>(kI64Max)) {
      step.overflow = true;
      step.known = false;
      plan.unobserved.push_back(t.resource);
      plan.complete = false;
      continue;
    }
    step.delta = -static_cast<std::int64_t>(excess);
    step.known = true;
    plan.any_compensation = true;
    plan.steps.push_back(step);
    (void)plan.target.put(t);
  }

  // A resource that was observed above target but is absent from the rollback
  // target is an inconsistent input, not a silently ignored entry.
  for (const RestorationAmount& o : observed_current.entries()) {
    if (!o.observed_known) {
      continue;
    }
    if (target.find(o.resource) == nullptr) {
      plan.unobserved.push_back(o.resource);
      plan.complete = false;
    }
  }

  return plan;
}

CheckedU64 total_effective(const RestorationVector& v) noexcept {
  CheckedU64 total{};
  for (const RestorationAmount& a : v.entries()) {
    const CheckedU64 next = add_u64(total.value, a.effective);
    if (next.overflow) {
      return next;
    }
    total.value = next.value;
  }
  return total;
}

}  // namespace congestion_recovery
