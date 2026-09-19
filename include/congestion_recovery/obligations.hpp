// Congestion Recovery - protected obligations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "congestion_recovery/enums.hpp"
#include "congestion_recovery/identities.hpp"
#include "congestion_recovery/restoration.hpp"

namespace congestion_recovery {

// A protected obligation is a constraint recovery may never violate. Hard
// obligations cannot be waived by any stage; soft ones are reported but do not
// block, and are labelled as such in explanations.
struct ProtectedObligation {
  ObligationId id{};
  ObligationGeneration generation{};
  ObligationKind kind{ObligationKind::UNKNOWN};
  ResourceId resource{};
  RestorationUnit unit{RestorationUnit::UNKNOWN};
  std::uint64_t threshold{0};
  std::uint64_t observed{0};
  bool observed_known{false};
  bool hard{true};
  bool active{true};
  ObligationState state{ObligationState::NOT_EVALUATED};
  std::string description{};
};

struct ObligationEvaluation {
  ObligationId id{};
  ObligationKind kind{ObligationKind::UNKNOWN};
  ResourceId resource{};
  ObligationState state{ObligationState::NOT_EVALUATED};
  std::uint64_t threshold{0};
  std::uint64_t candidate{0};
  bool hard{true};
  bool blocking{false};
  std::string detail{};
};

// Evaluate one obligation against a candidate level for its resource.
[[nodiscard]] ObligationEvaluation evaluate_obligation(const ProtectedObligation& obligation,
                                                       const RestorationVector& candidate) noexcept;

}  // namespace congestion_recovery
