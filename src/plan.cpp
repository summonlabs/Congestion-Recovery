// Congestion Recovery - plan helpers.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/plan.hpp"

namespace congestion_recovery {
namespace {

void mix(std::uint64_t& h, std::uint64_t v) noexcept {
  // FNV-1a over the 8 bytes of v, stable across platforms.
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xffull;
    h *= 0x100000001B3ull;
  }
}

}  // namespace

std::uint64_t completion_key(RecoveryPlanId plan, PlanGeneration plan_generation,
                             StageGeneration stage_generation, std::uint64_t evidence_generation,
                             AttemptId attempt) noexcept {
  std::uint64_t h = 0xCBF29CE484222325ull;
  mix(h, plan.value());
  mix(h, plan_generation.value());
  mix(h, stage_generation.value());
  mix(h, evidence_generation);
  mix(h, attempt.value());
  return h;
}

}  // namespace congestion_recovery
