// Congestion Recovery - recovery authority vector.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"

namespace congestion_recovery {

// A grant of legal restoration headroom for one resource, issued under one
// coordinator epoch. Authority is live state: it is never silently restored
// from durable storage.
struct RecoveryAuthority {
  AuthorityId id{};
  AuthorityGeneration generation{};
  CoordinatorEpoch epoch{};
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::uint64_t ceiling{0};
  std::uint64_t floor{0};
  std::uint64_t granted_at_tick{0};
  std::uint64_t expires_at_tick{0};
  bool revocable{true};
  PublisherId issuer{};
  std::string note{};
};

struct AuthorityView {
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  AuthorityGeneration generation{};
  std::uint64_t ceiling{0};
  std::uint64_t floor{0};
  bool present{false};
  bool expired{false};
  bool epoch_match{false};
};

// Ordered by resource identity; one live grant per resource.
class AuthorityVector {
 public:
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const std::vector<RecoveryAuthority>& entries() const noexcept { return entries_; }
  void clear() noexcept { entries_.clear(); }

  bool put(const RecoveryAuthority& a);
  void remove(ResourceId resource);
  [[nodiscard]] const RecoveryAuthority* find(ResourceId resource) const noexcept;
  [[nodiscard]] bool structurally_valid() const noexcept;

  // Drop every grant that is expired at now_tick or bound to another epoch.
  // Returns the number of grants dropped.
  std::size_t drop_stale(CoordinatorEpoch epoch, std::uint64_t now_tick) noexcept;

 private:
  std::vector<RecoveryAuthority> entries_{};
};

}  // namespace congestion_recovery
