// Congestion Recovery - authority vector operations.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/authority.hpp"

#include <algorithm>

namespace congestion_recovery {

bool AuthorityVector::put(const RecoveryAuthority& a) {
  if (!a.id.is_valid() || !a.generation.is_valid() || !a.resource.is_valid() || !is_valid(a.unit)) {
    return false;
  }
  if (a.ceiling < a.floor) {
    return false;
  }
  if (a.expires_at_tick < a.granted_at_tick) {
    return false;
  }
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), a.resource,
                                   [](const RecoveryAuthority& e, ResourceId r) { return e.resource < r; });
  if (it != entries_.end() && it->resource == a.resource) {
    // A replacement grant must not regress its generation: that would let a
    // revoked authority silently return.
    if (a.generation <= it->generation && a.epoch <= it->epoch) {
      return false;
    }
    *it = a;
    return true;
  }
  entries_.insert(it, a);
  return true;
}

void AuthorityVector::remove(ResourceId resource) {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), resource,
                                   [](const RecoveryAuthority& e, ResourceId r) { return e.resource < r; });
  if (it != entries_.end() && it->resource == resource) {
    entries_.erase(it);
  }
}

const RecoveryAuthority* AuthorityVector::find(ResourceId resource) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), resource,
                                   [](const RecoveryAuthority& e, ResourceId r) { return e.resource < r; });
  if (it == entries_.end() || !(it->resource == resource)) {
    return nullptr;
  }
  return &(*it);
}

bool AuthorityVector::structurally_valid() const noexcept {
  ResourceId previous{};
  bool first = true;
  for (const RecoveryAuthority& a : entries_) {
    if (!a.id.is_valid() || !a.generation.is_valid() || !a.resource.is_valid() || !is_valid(a.unit)) {
      return false;
    }
    if (a.ceiling < a.floor || a.expires_at_tick < a.granted_at_tick) {
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

std::size_t AuthorityVector::drop_stale(CoordinatorEpoch epoch, std::uint64_t now_tick) noexcept {
  const std::size_t before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [epoch, now_tick](const RecoveryAuthority& a) {
                                  if (!(a.epoch == epoch)) {
                                    return true;
                                  }
                                  return a.expires_at_tick <= now_tick;
                                }),
                 entries_.end());
  return before - entries_.size();
}

}  // namespace congestion_recovery
