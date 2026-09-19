// Congestion Recovery - restoration levels, vectors and compensating plans.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/checked.hpp"
#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"

namespace congestion_recovery {

// One resource's restoration entry. All numeric fields are ABSOLUTE LEVELS in
// the resource's declared unit, never deltas:
//   requested  - the level the stage asked for
//   authorized - the level current legal authority permits
//   effective  - min(requested, authorized) after obligation clamping
//   observed   - the level actually observed as applied (observed_known gate)
struct RestorationAmount {
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::uint64_t requested{0};
  std::uint64_t authorized{0};
  std::uint64_t effective{0};
  std::uint64_t observed{0};
  bool observed_known{false};
  // Set when legal authority (not the stage target) determined the effective
  // level. Obligations never silently clamp: they reject.
  bool authority_clamped{false};
};

// A signed per-resource delta with explicit knowledge tracking.
struct RestorationDelta {
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::int64_t delta{0};
  bool known{false};
  bool overflow{false};
};

// Ordered, duplicate-free collection of restoration levels. Ordering is by
// resource identity so that serialization and comparison are deterministic.
class RestorationVector {
 public:
  RestorationVector() = default;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const std::vector<RestorationAmount>& entries() const noexcept { return entries_; }

  void clear() noexcept { entries_.clear(); }

  // Inserts or updates the entry for a resource. Returns false when the
  // resource identity is invalid or the unit is UNKNOWN.
  bool set_level(ResourceId resource, RestorationUnit unit, std::uint64_t level) {
    if (!resource.is_valid() || !is_valid(unit)) {
      return false;
    }
    RestorationAmount* existing = find_mut(resource);
    if (existing != nullptr) {
      if (existing->unit != unit) {
        return false;
      }
      existing->requested = level;
      existing->authorized = level;
      existing->effective = level;
      existing->authority_clamped = false;
      return true;
    }
    RestorationAmount a{};
    a.resource = resource;
    a.unit = unit;
    a.requested = level;
    a.authorized = level;
    a.effective = level;
    insert_sorted(a);
    return true;
  }

  // Full insert used by deserialization; rejects duplicates and bad units.
  bool put(const RestorationAmount& a) {
    if (!a.resource.is_valid() || !is_valid(a.unit)) {
      return false;
    }
    if (find(a.resource) != nullptr) {
      return false;
    }
    insert_sorted(a);
    return true;
  }

  [[nodiscard]] const RestorationAmount* find(ResourceId resource) const noexcept {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), resource,
                                     [](const RestorationAmount& a, ResourceId r) { return a.resource < r; });
    if (it == entries_.end() || !(it->resource == resource)) {
      return nullptr;
    }
    return &(*it);
  }

  [[nodiscard]] RestorationAmount* find_mut(ResourceId resource) noexcept {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), resource,
                                     [](const RestorationAmount& a, ResourceId r) { return a.resource < r; });
    if (it == entries_.end() || !(it->resource == resource)) {
      return nullptr;
    }
    return &(*it);
  }

  [[nodiscard]] const RestorationAmount* at(std::size_t index) const noexcept {
    return index < entries_.size() ? &entries_[index] : nullptr;
  }

  [[nodiscard]] bool has(ResourceId resource) const noexcept { return find(resource) != nullptr; }

  [[nodiscard]] std::uint64_t effective_level(ResourceId resource) const noexcept {
    const RestorationAmount* a = find(resource);
    return a != nullptr ? a->effective : 0u;
  }

  [[nodiscard]] std::uint64_t requested_level(ResourceId resource) const noexcept {
    const RestorationAmount* a = find(resource);
    return a != nullptr ? a->requested : 0u;
  }

  [[nodiscard]] RestorationUnit unit_of(ResourceId resource) const noexcept {
    const RestorationAmount* a = find(resource);
    return a != nullptr ? a->unit : RestorationUnit::UNKNOWN;
  }

  // Structural validity: sorted, unique, valid identities and units.
  [[nodiscard]] bool structurally_valid() const noexcept {
    ResourceId previous{};
    bool first = true;
    for (const RestorationAmount& a : entries_) {
      if (!a.resource.is_valid() || !is_valid(a.unit)) {
        return false;
      }
      if (!first && !(previous < a.resource)) {
        return false;
      }
      previous = a.resource;
      first = false;
    }
    return true;
  }

  // Signed per-resource difference (this minus other). Resources missing from
  // either side yield known == false.
  [[nodiscard]] std::vector<RestorationDelta> difference(const RestorationVector& other) const {
    std::vector<RestorationDelta> out;
    out.reserve(entries_.size());
    for (const RestorationAmount& a : entries_) {
      RestorationDelta d{};
      d.resource = a.resource;
      d.unit = a.unit;
      const RestorationAmount* b = other.find(a.resource);
      if (b == nullptr) {
        d.known = false;
        out.push_back(d);
        continue;
      }
      const CheckedI64 diff = sub_i64(a.effective, b->effective);
      d.delta = diff.value;
      d.overflow = diff.overflow;
      d.known = !diff.overflow;
      out.push_back(d);
    }
    return out;
  }

  [[nodiscard]] bool equals(const RestorationVector& other) const noexcept {
    if (entries_.size() != other.entries_.size()) {
      return false;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const RestorationAmount& a = entries_[i];
      const RestorationAmount& b = other.entries_[i];
      if (!(a.resource == b.resource) || a.unit != b.unit || a.effective != b.effective) {
        return false;
      }
    }
    return true;
  }

 private:
  void insert_sorted(const RestorationAmount& a) {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), a.resource,
                                     [](const RestorationAmount& e, ResourceId r) { return e.resource < r; });
    entries_.insert(it, a);
  }

  std::vector<RestorationAmount> entries_{};
};

// A compensating plan is an explicit artifact, never an implicit reversal.
struct CompensatingPlan {
  std::vector<RestorationDelta> steps{};       // reduce resource level by |delta|
  RestorationVector target{};                  // level each resource must reach
  std::vector<ResourceId> unobserved{};        // observed state unknown: cannot be computed
  std::vector<ResourceId> already_at_target{}; // nothing to do
  bool complete{false};                        // every resource has an observed level
  bool any_compensation{false};
};

// Recompute a compensating plan from OBSERVED current levels toward target.
// This never assumes previously requested restoration steps succeeded: only
// observed levels participate, and unobserved resources are reported as such.
[[nodiscard]] CompensatingPlan build_compensating_plan(const RestorationVector& observed_current,
                                                       const RestorationVector& target);

// Sum of effective levels; saturating with an explicit overflow flag.
[[nodiscard]] CheckedU64 total_effective(const RestorationVector& v) noexcept;

}  // namespace congestion_recovery
