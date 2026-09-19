// Congestion Recovery - strongly typed identities, generations and epochs.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include "congestion_recovery/checked.hpp"

namespace congestion_recovery {

// Every external handle is a distinct type. A RecoveryPlanId can never be
// passed where a ResourceId is expected, and no identity has a meaningful
// default: value 0 is always INVALID.
template <class Tag>
class StrongId {
 public:
  using value_type = std::uint64_t;

  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(std::uint64_t v) noexcept : value_(v) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0u; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0u; }
  constexpr void reset() noexcept { value_ = 0u; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept = default;
  friend constexpr auto operator<=>(StrongId a, StrongId b) noexcept = default;

 private:
  std::uint64_t value_{0};
};

// Generations only ever move forward. Bumping past the maximum fails and leaves
// the generation unchanged instead of wrapping.
template <class Tag>
class Generation {
 public:
  using value_type = std::uint64_t;

  constexpr Generation() noexcept = default;
  explicit constexpr Generation(std::uint64_t v) noexcept : value_(v) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0u; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0u; }

  // Returns false when the generation cannot advance without wrapping.
  constexpr bool bump() noexcept {
    if (value_ == kU64Max) {
      return false;
    }
    ++value_;
    return true;
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept = default;
  friend constexpr auto operator<=>(Generation a, Generation b) noexcept = default;

 private:
  std::uint64_t value_{0};
};

template <class T>
struct StrongKeyHash {
  [[nodiscard]] std::size_t operator()(const T& v) const noexcept {
    std::uint64_t x = v.value();
    // splitmix64 finalizer: stable across platforms and standard libraries.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return static_cast<std::size_t>(x);
  }
};

// ---- identity tags -------------------------------------------------------
struct RecoveryPlanTag;
struct PolicyTag;
struct CongestionRefTag;
struct InterventionTag;
struct ResourceTag;
struct EvidenceTag;
struct EvidenceSnapshotTag;
struct StageTag;
struct AttemptTag;
struct RollbackTag;
struct CompletionTag;
struct ObligationTag;
struct AuthorityTag;
struct WorkerTag;
struct PublisherTag;
struct JournalTag;
struct CoordinatorEpochTag;
struct WorkerBootTag;
struct PublisherIncarnationTag;

// ---- identities ----------------------------------------------------------
using RecoveryPlanId = StrongId<RecoveryPlanTag>;
using PolicyId = StrongId<PolicyTag>;
using CongestionRefId = StrongId<CongestionRefTag>;
using InterventionId = StrongId<InterventionTag>;
using ResourceId = StrongId<ResourceTag>;
using EvidenceId = StrongId<EvidenceTag>;
using EvidenceSnapshotId = StrongId<EvidenceSnapshotTag>;
using StageId = StrongId<StageTag>;
using AttemptId = StrongId<AttemptTag>;
using RollbackId = StrongId<RollbackTag>;
using CompletionId = StrongId<CompletionTag>;
using ObligationId = StrongId<ObligationTag>;
using AuthorityId = StrongId<AuthorityTag>;
using WorkerId = StrongId<WorkerTag>;
using PublisherId = StrongId<PublisherTag>;
using JournalId = StrongId<JournalTag>;

// Coordinator epoch: bumped on every coordinator start and on every explicit
// epoch advance. Work bound to an older epoch is stale by construction.
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;

// Worker boot incarnation: a restarted worker is a different authority holder.
// Boots are strictly increasing per worker; a regression is rejected.
using WorkerBoot = StrongId<WorkerBootTag>;

// Publisher incarnation follows the same rule for evidence publishers.
using PublisherIncarnation = StrongId<PublisherIncarnationTag>;

// ---- generations ---------------------------------------------------------
using PlanGeneration = Generation<RecoveryPlanTag>;
using PolicyGeneration = Generation<PolicyTag>;
using ResourceGeneration = Generation<ResourceTag>;
using EvidenceGeneration = Generation<EvidenceTag>;
using SnapshotGeneration = Generation<EvidenceSnapshotTag>;
using StageGeneration = Generation<StageTag>;
using AttemptGeneration = Generation<AttemptTag>;
using RollbackGeneration = Generation<RollbackTag>;
using CompletionGeneration = Generation<CompletionTag>;
using ObligationGeneration = Generation<ObligationTag>;
using AuthorityGeneration = Generation<AuthorityTag>;
using JournalGeneration = Generation<JournalTag>;

}  // namespace congestion_recovery
