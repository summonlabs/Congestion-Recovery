// Congestion Recovery - explicit adjacent-runtime requests.
// Copyright 2026 Summon Software Labs.
//
// Congestion Recovery never performs congestion detection, admission, bandwidth
// arbitration, rate enforcement, pacing, queue/buffer management, path
// computation, traffic-engineering allocation, backpressure propagation or
// physical forwarding. It emits bounded, generation-bound requests that the
// owning runtime may accept or reject.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/policy.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

struct AdjacentRequest {
  AdjacentRequestKind kind{AdjacentRequestKind::UNKNOWN};
  RecoveryPlanId plan{};
  PlanGeneration plan_generation{};
  StageGeneration stage_generation{};
  CoordinatorEpoch epoch{};
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::uint64_t amount{0};
  std::uint64_t issued_at_tick{0};
  std::string owner_runtime{};
  std::string detail{};
};

// Bounded queue of outbound requests. Congestion Recovery records intent only;
// it never claims the request was applied.
class AdjacentRequestLog {
 public:
  explicit AdjacentRequestLog(std::uint32_t capacity) noexcept
      : capacity_(capacity == 0u ? 1u : capacity) {}

  bool push(const AdjacentRequest& request);
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] const std::vector<AdjacentRequest>& entries() const noexcept { return entries_; }
  void clear() noexcept { entries_.clear(); }

  // Deterministic single-line rendering used by explanations and tools.
  [[nodiscard]] std::vector<std::string> render(std::uint32_t limit) const;

 private:
  std::uint32_t capacity_{1};
  std::uint64_t dropped_{0};
  std::vector<AdjacentRequest> entries_{};
};

}  // namespace congestion_recovery
