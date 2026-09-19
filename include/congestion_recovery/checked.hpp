// Congestion Recovery - checked arithmetic for externally influenced quantities.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <limits>

namespace congestion_recovery {

// Result of a checked operation. When overflow is true the value field is a
// saturated placeholder and must not be treated as authoritative.
struct CheckedU64 {
  std::uint64_t value{0};
  bool overflow{false};
  [[nodiscard]] constexpr bool ok() const noexcept { return !overflow; }
};

struct CheckedI64 {
  std::int64_t value{0};
  bool overflow{false};
  [[nodiscard]] constexpr bool ok() const noexcept { return !overflow; }
};

inline constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
inline constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] constexpr CheckedU64 add_u64(std::uint64_t a, std::uint64_t b) noexcept {
  CheckedU64 r{};
  r.value = a + b;
  r.overflow = r.value < a;
  if (r.overflow) {
    r.value = kU64Max;
  }
  return r;
}

// Saturating subtraction: an underflow is reported, never wrapped.
[[nodiscard]] constexpr CheckedU64 sub_u64(std::uint64_t a, std::uint64_t b) noexcept {
  CheckedU64 r{};
  r.overflow = a < b;
  r.value = r.overflow ? 0u : (a - b);
  return r;
}

[[nodiscard]] constexpr CheckedU64 mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  CheckedU64 r{};
  if (a == 0u || b == 0u) {
    return r;
  }
  if (a > (kU64Max / b)) {
    r.overflow = true;
    r.value = kU64Max;
    return r;
  }
  r.value = a * b;
  return r;
}

// value * basis_points / 10000 computed without intermediate overflow.
[[nodiscard]] constexpr CheckedU64 pct_bp_u64(std::uint64_t value, std::uint64_t basis_points) noexcept {
  CheckedU64 r{};
  const std::uint64_t whole = value / 10000u;
  const std::uint64_t rem = value % 10000u;
  const CheckedU64 hi = mul_u64(whole, basis_points);
  if (hi.overflow) {
    r.overflow = true;
    r.value = kU64Max;
    return r;
  }
  const CheckedU64 lo = mul_u64(rem, basis_points);
  if (lo.overflow) {
    r.overflow = true;
    r.value = kU64Max;
    return r;
  }
  const CheckedU64 sum = add_u64(hi.value, lo.value / 10000u);
  if (sum.overflow) {
    r.overflow = true;
    r.value = kU64Max;
    return r;
  }
  r.value = sum.value;
  return r;
}

// a - b as a signed delta with explicit overflow detection.
[[nodiscard]] constexpr CheckedI64 sub_i64(std::uint64_t a, std::uint64_t b) noexcept {
  CheckedI64 r{};
  if (a >= b) {
    const std::uint64_t d = a - b;
    if (d > static_cast<std::uint64_t>(kI64Max)) {
      r.overflow = true;
      r.value = kI64Max;
      return r;
    }
    r.value = static_cast<std::int64_t>(d);
    return r;
  }
  const std::uint64_t d = b - a;
  const std::uint64_t limit = static_cast<std::uint64_t>(kI64Max) + 1u;
  if (d > limit) {
    r.overflow = true;
    r.value = kI64Min;
    return r;
  }
  if (d == limit) {
    r.value = kI64Min;
    return r;
  }
  r.value = -static_cast<std::int64_t>(d);
  return r;
}

[[nodiscard]] constexpr std::uint64_t clamp_u64(std::uint64_t v, std::uint64_t lo, std::uint64_t hi) noexcept {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

[[nodiscard]] constexpr std::uint64_t min_u64(std::uint64_t a, std::uint64_t b) noexcept {
  return a < b ? a : b;
}

[[nodiscard]] constexpr std::uint64_t max_u64(std::uint64_t a, std::uint64_t b) noexcept {
  return a > b ? a : b;
}

// Tick arithmetic is externally influenced (callers supply tick values), so
// every addition is checked and saturating.
[[nodiscard]] constexpr std::uint64_t tick_add(std::uint64_t a, std::uint64_t b) noexcept {
  return add_u64(a, b).value;
}

}  // namespace congestion_recovery
