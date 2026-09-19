// Congestion Recovery - versioned snapshot serialization and journal replay.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/journal.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace congestion_recovery {
namespace {

constexpr std::uint32_t kSnapshotMagic = 0x54535243u;  // "CRST"
constexpr std::uint32_t kJournalMagic = 0x524A5243u;   // "CRJR"
constexpr std::size_t kSnapshotHeaderBytes = 4u + 4u + 8u;
constexpr std::size_t kJournalHeaderBytes = 4u + 4u;
constexpr std::uint32_t kMaxDurableStringBytes = 4096u;

bool set_err(std::string& err, const char* text) {
  err = text;
  return false;
}

// Bounded little-endian writer used only for durable state.
class Out {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }
  }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void f64(double v) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
  }
  void str(const std::string& v) {
    const std::uint32_t size = static_cast<std::uint32_t>(v.size() & 0xffffffffu);
    u32(size);
    buf_.insert(buf_.end(), v.begin(), v.end());
  }
  void bytes(const std::vector<std::uint8_t>& v) {
    u32(static_cast<std::uint32_t>(v.size() & 0xffffffffu));
    buf_.insert(buf_.end(), v.begin(), v.end());
  }
  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buf_; }
  [[nodiscard]] std::vector<std::uint8_t>& buffer() noexcept { return buf_; }

 private:
  std::vector<std::uint8_t> buf_{};
};

// Bounded little-endian reader. Any read past the end marks the reader bad.
class In {
 public:
  In(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] std::uint8_t u8() {
    if (!need(1)) {
      return 0;
    }
    return data_[off_++];
  }
  [[nodiscard]] std::uint32_t u32() {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    off_ += 4;
    return v;
  }
  [[nodiscard]] std::uint64_t u64() {
    if (!need(8)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(data_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    off_ += 8;
    return v;
  }
  [[nodiscard]] std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
  [[nodiscard]] double f64() {
    const std::uint64_t bits = u64();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  [[nodiscard]] std::string str() {
    const std::uint32_t size = u32();
    if (bad_ || size > kMaxDurableStringBytes) {
      bad_ = true;
      return {};
    }
    if (!need(size)) {
      bad_ = true;
      return {};
    }
    std::string v(reinterpret_cast<const char*>(data_ + off_), size);
    off_ += size;
    return v;
  }
  [[nodiscard]] std::vector<std::uint8_t> bytes() {
    const std::uint32_t size = u32();
    if (bad_ || size > kSnapshotMaxBytes) {
      bad_ = true;
      return {};
    }
    if (!need(size)) {
      bad_ = true;
      return {};
    }
    std::vector<std::uint8_t> v(data_ + off_, data_ + off_ + size);
    off_ += size;
    return v;
  }
  [[nodiscard]] bool bad() const noexcept { return bad_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - off_; }

 private:
  [[nodiscard]] bool need(std::size_t n) noexcept {
    if (bad_ || n > size_ - off_) {
      bad_ = true;
      return false;
    }
    return true;
  }

  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t off_{0};
  bool bad_{false};
};

void write_amount(Out& out, const RestorationAmount& a) {
  out.u64(a.resource.value());
  out.u8(static_cast<std::uint8_t>(a.unit));
  out.u64(a.requested);
  out.u64(a.authorized);
  out.u64(a.effective);
  out.u64(a.observed);
  out.u8(a.observed_known ? 1u : 0u);
  out.u8(a.authority_clamped ? 1u : 0u);
}

bool read_amount(In& in, RestorationAmount& a) {
  a.resource = ResourceId(in.u64());
  a.unit = static_cast<RestorationUnit>(in.u8());
  a.requested = in.u64();
  a.authorized = in.u64();
  a.effective = in.u64();
  a.observed = in.u64();
  a.observed_known = in.u8() != 0u;
  a.authority_clamped = in.u8() != 0u;
  return !in.bad();
}

void write_vector(Out& out, const RestorationVector& v) {
  out.u32(static_cast<std::uint32_t>(v.size()));
  for (const RestorationAmount& a : v.entries()) {
    write_amount(out, a);
  }
}

bool read_vector(In& in, std::uint32_t max_items, RestorationVector& v) {
  const std::uint32_t count = in.u32();
  if (in.bad() || count > max_items) {
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    RestorationAmount a{};
    if (!read_amount(in, a)) {
      return false;
    }
    if (!is_valid(a.unit)) {
      return false;
    }
    if (!v.put(a)) {
      return false;
    }
  }
  return true;
}

void write_policy(Out& out, const RecoveryPolicy& p) {
  out.u64(p.id.value());
  out.u64(p.generation.value());
  out.str(p.name);
  out.u32(static_cast<std::uint32_t>(p.stages.size()));
  for (const StageSpec& s : p.stages) {
    out.u8(static_cast<std::uint8_t>(s.kind));
    out.str(s.name);
    write_vector(out, s.target);
    out.u64(s.dwell_ticks);
    out.u64(s.stability_window_ticks);
    out.u32(s.max_attempts);
    out.u8(s.requires_authority ? 1u : 0u);
    out.u8(s.rollback_on_failure ? 1u : 0u);
    out.u32(static_cast<std::uint32_t>(s.requirements.size()));
    for (const EvidenceRequirement& r : s.requirements) {
      out.u8(static_cast<std::uint8_t>(r.kind));
      out.u8(static_cast<std::uint8_t>(r.min_provenance));
      out.f64(r.min_confidence);
      out.u64(r.min_samples);
      out.u64(r.min_duration_ticks);
      out.u64(r.stability_window_ticks);
      out.u64(r.min_value);
      out.u64(r.max_value);
      std::uint8_t flags = 0;
      if (r.has_min_value) flags |= 0x01u;
      if (r.has_max_value) flags |= 0x02u;
      if (r.require_stable) flags |= 0x04u;
      if (r.require_affirmative) flags |= 0x08u;
      if (r.require_bound_resource) flags |= 0x10u;
      if (r.primary) flags |= 0x20u;
      out.u8(flags);
    }
  }
  out.u64(p.hysteresis.advance_margin);
  out.u64(p.hysteresis.rollback_margin);
  out.u64(p.hysteresis.cooldown_ticks);
  out.u32(p.hysteresis.max_transitions_per_window);
  out.u64(p.hysteresis.window_ticks);
  out.u8(static_cast<std::uint8_t>(p.on_recurrence));
  out.u64(p.max_evidence_age_ticks);
  out.u64(p.max_plan_lifetime_ticks);
  out.u64(p.revalidation_boundary_ticks);
  out.u32(p.max_rollbacks);
  out.u32(p.max_total_attempts);
  out.u32(p.partial_application_tolerance_bp);
  std::uint8_t flags = 0;
  if (p.baseline_required) flags |= 0x01u;
  if (p.require_completion_evidence) flags |= 0x02u;
  if (p.require_stable_completion) flags |= 0x04u;
  out.u8(flags);
}

bool read_policy(In& in, std::uint32_t max_stages, std::uint32_t max_requirements, RecoveryPolicy& p) {
  p.id = PolicyId(in.u64());
  p.generation = PolicyGeneration(in.u64());
  p.name = in.str();
  const std::uint32_t stage_count = in.u32();
  if (in.bad() || stage_count > max_stages) {
    return false;
  }
  p.stages.clear();
  p.stages.reserve(stage_count);
  for (std::uint32_t i = 0; i < stage_count; ++i) {
    StageSpec s{};
    s.kind = static_cast<StageKind>(in.u8());
    s.name = in.str();
    if (!read_vector(in, 4096u, s.target)) {
      return false;
    }
    s.dwell_ticks = in.u64();
    s.stability_window_ticks = in.u64();
    s.max_attempts = in.u32();
    s.requires_authority = in.u8() != 0u;
    s.rollback_on_failure = in.u8() != 0u;
    const std::uint32_t req_count = in.u32();
    if (in.bad() || req_count > max_requirements) {
      return false;
    }
    for (std::uint32_t j = 0; j < req_count; ++j) {
      EvidenceRequirement r{};
      r.kind = static_cast<EvidenceKind>(in.u8());
      r.min_provenance = static_cast<Provenance>(in.u8());
      r.min_confidence = in.f64();
      r.min_samples = in.u64();
      r.min_duration_ticks = in.u64();
      r.stability_window_ticks = in.u64();
      r.min_value = in.u64();
      r.max_value = in.u64();
      const std::uint8_t flags = in.u8();
      r.has_min_value = (flags & 0x01u) != 0u;
      r.has_max_value = (flags & 0x02u) != 0u;
      r.require_stable = (flags & 0x04u) != 0u;
      r.require_affirmative = (flags & 0x08u) != 0u;
      r.require_bound_resource = (flags & 0x10u) != 0u;
      r.primary = (flags & 0x20u) != 0u;
      if (!is_valid(r.kind) || r.min_confidence < 0.0 || r.min_confidence > 1.0) {
        return false;
      }
      s.requirements.push_back(r);
    }
    if (!is_valid(s.kind)) {
      return false;
    }
    p.stages.push_back(std::move(s));
  }
  p.hysteresis.advance_margin = in.u64();
  p.hysteresis.rollback_margin = in.u64();
  p.hysteresis.cooldown_ticks = in.u64();
  p.hysteresis.max_transitions_per_window = in.u32();
  p.hysteresis.window_ticks = in.u64();
  p.on_recurrence = static_cast<RecurrenceAction>(in.u8());
  p.max_evidence_age_ticks = in.u64();
  p.max_plan_lifetime_ticks = in.u64();
  p.revalidation_boundary_ticks = in.u64();
  p.max_rollbacks = in.u32();
  p.max_total_attempts = in.u32();
  p.partial_application_tolerance_bp = in.u32();
  const std::uint8_t flags = in.u8();
  p.baseline_required = (flags & 0x01u) != 0u;
  p.require_completion_evidence = (flags & 0x02u) != 0u;
  p.require_stable_completion = (flags & 0x04u) != 0u;
  return !in.bad();
}

void write_plan(Out& out, const RecoveryPlan& p) {
  out.u64(p.id.value());
  out.u64(p.generation.value());
  out.u64(p.epoch.value());
  out.u64(p.policy_id.value());
  out.u64(p.policy_generation.value());
  out.u64(p.congestion.value());
  out.u64(p.intervention.value());
  out.u32(static_cast<std::uint32_t>(p.resources.size()));
  for (const ResourceBinding& b : p.resources) {
    out.u64(b.resource.value());
    out.u64(b.generation.value());
  }
  write_vector(out, p.baseline);
  out.u8(p.baseline_known ? 1u : 0u);
  write_vector(out, p.constrained);
  write_vector(out, p.rollback_target);
  write_vector(out, p.authorized);
  out.u32(static_cast<std::uint32_t>(p.obligations.size()));
  for (const ProtectedObligation& o : p.obligations) {
    out.u64(o.id.value());
    out.u64(o.generation.value());
    out.u8(static_cast<std::uint8_t>(o.kind));
    out.u64(o.resource.value());
    out.u8(static_cast<std::uint8_t>(o.unit));
    out.u64(o.threshold);
    out.u64(o.observed);
    out.u8(o.observed_known ? 1u : 0u);
    out.u8(o.hard ? 1u : 0u);
    out.u8(o.active ? 1u : 0u);
    out.u8(static_cast<std::uint8_t>(o.state));
    out.str(o.description);
  }
  out.u8(static_cast<std::uint8_t>(p.state));
  out.u64(p.stage_index);
  out.u64(p.stage_generation.value());
  out.u64(p.attempt.value());
  out.u64(p.created_at_tick);
  out.u64(p.updated_at_tick);
  out.u64(p.last_transition_tick);
  out.u64(p.cooldown_until_tick);
  out.u32(p.attempts_in_stage);
  out.u32(p.total_attempts);
  out.u32(p.rollback_count);
  out.u32(p.completion_count);
  out.u8(static_cast<std::uint8_t>(p.last_reason));
  out.str(p.last_reason_text);
  out.u8(p.revalidation_required ? 1u : 0u);
  out.u32(static_cast<std::uint32_t>(p.revalidation_notes.size()));
  for (const std::string& note : p.revalidation_notes) {
    out.str(note);
  }
  out.u32(static_cast<std::uint32_t>(p.stage_history.size()));
  for (const StageRecord& r : p.stage_history) {
    out.u64(r.index);
    out.u8(static_cast<std::uint8_t>(r.kind));
    out.u64(r.generation.value());
    out.u64(r.attempt.value());
    out.u64(r.entered_tick);
    out.u64(r.exited_tick);
    out.u8(static_cast<std::uint8_t>(r.outcome));
    out.u32(r.attempts);
    out.u64(r.evidence_generation.value());
    out.u64(r.authority_generation.value());
    out.u64(r.epoch.value());
    write_vector(out, r.requested);
    write_vector(out, r.authorized);
    write_vector(out, r.observed);
    out.u8(static_cast<std::uint8_t>(r.last_reason));
  }
  out.u32(static_cast<std::uint32_t>(p.rollback_history.size()));
  for (const RollbackRecord& r : p.rollback_history) {
    out.u64(r.id.value());
    out.u64(r.generation.value());
    out.u64(r.epoch.value());
    out.u8(static_cast<std::uint8_t>(r.kind));
    out.u8(static_cast<std::uint8_t>(r.trigger));
    out.u64(r.tick);
    write_vector(out, r.observed_current);
    write_vector(out, r.target);
    write_vector(out, r.compensation);
    out.u32(static_cast<std::uint32_t>(r.unobserved.size()));
    for (const ResourceId id : r.unobserved) {
      out.u64(id.value());
    }
    out.u32(r.assumed_steps);
    out.u32(r.compensation_steps);
    out.u32(r.observed_steps);
    out.u8(r.complete ? 1u : 0u);
    out.str(r.reason);
  }
  out.u32(static_cast<std::uint32_t>(p.completions.size()));
  for (const CompletionRecord& r : p.completions) {
    out.u64(r.id.value());
    out.u64(r.generation.value());
    out.u64(r.epoch.value());
    out.u64(r.stage_index);
    out.u64(r.stage_generation.value());
    out.u64(r.attempt.value());
    out.u64(r.evidence_generation.value());
    out.u8(static_cast<std::uint8_t>(r.state));
    out.u64(r.recorded_tick);
    out.u64(r.idempotency_key);
    write_vector(out, r.applied);
    out.u8(static_cast<std::uint8_t>(r.reject));
    out.u8(r.replayed ? 1u : 0u);
  }
  out.u32(static_cast<std::uint32_t>(p.transition_ticks.size()));
  for (const std::uint64_t tick : p.transition_ticks) {
    out.u64(tick);
  }
  write_vector(out, p.last_observed);
}

bool read_plan(In& in, const EngineLimits& limits, RecoveryPlan& p) {
  p.id = RecoveryPlanId(in.u64());
  p.generation = PlanGeneration(in.u64());
  p.epoch = CoordinatorEpoch(in.u64());
  p.policy_id = PolicyId(in.u64());
  p.policy_generation = PolicyGeneration(in.u64());
  p.congestion = CongestionRefId(in.u64());
  p.intervention = InterventionId(in.u64());
  const std::uint32_t resource_count = in.u32();
  if (in.bad() || resource_count > limits.max_resources_per_plan) {
    return false;
  }
  p.resources.clear();
  p.resources.reserve(resource_count);
  for (std::uint32_t i = 0; i < resource_count; ++i) {
    ResourceBinding b{};
    b.resource = ResourceId(in.u64());
    b.generation = ResourceGeneration(in.u64());
    if (!b.resource.is_valid() || !b.generation.is_valid()) {
      return false;
    }
    p.resources.push_back(b);
  }
  if (!read_vector(in, limits.max_resources_per_plan, p.baseline)) {
    return false;
  }
  p.baseline_known = in.u8() != 0u;
  if (!read_vector(in, limits.max_resources_per_plan, p.constrained)) {
    return false;
  }
  if (!read_vector(in, limits.max_resources_per_plan, p.rollback_target)) {
    return false;
  }
  if (!read_vector(in, limits.max_resources_per_plan, p.authorized)) {
    return false;
  }
  const std::uint32_t obligation_count = in.u32();
  if (in.bad() || obligation_count > limits.max_obligations_per_plan) {
    return false;
  }
  for (std::uint32_t i = 0; i < obligation_count; ++i) {
    ProtectedObligation o{};
    o.id = ObligationId(in.u64());
    o.generation = ObligationGeneration(in.u64());
    o.kind = static_cast<ObligationKind>(in.u8());
    o.resource = ResourceId(in.u64());
    o.unit = static_cast<RestorationUnit>(in.u8());
    o.threshold = in.u64();
    o.observed = in.u64();
    o.observed_known = in.u8() != 0u;
    o.hard = in.u8() != 0u;
    o.active = in.u8() != 0u;
    o.state = static_cast<ObligationState>(in.u8());
    o.description = in.str();
    if (!o.id.is_valid() || !is_valid(o.kind) || !is_valid(o.unit) || !o.resource.is_valid() ||
        !is_valid(o.state)) {
      return false;
    }
    p.obligations.push_back(std::move(o));
  }
  p.state = static_cast<RecoveryState>(in.u8());
  p.stage_index = static_cast<std::size_t>(in.u64());
  p.stage_generation = StageGeneration(in.u64());
  p.attempt = AttemptId(in.u64());
  p.created_at_tick = in.u64();
  p.updated_at_tick = in.u64();
  p.last_transition_tick = in.u64();
  p.cooldown_until_tick = in.u64();
  p.attempts_in_stage = in.u32();
  p.total_attempts = in.u32();
  p.rollback_count = in.u32();
  p.completion_count = in.u32();
  p.last_reason = static_cast<RejectReason>(in.u8());
  p.last_reason_text = in.str();
  p.revalidation_required = in.u8() != 0u;
  const std::uint32_t note_count = in.u32();
  if (in.bad() || note_count > limits.max_explanation_items) {
    return false;
  }
  for (std::uint32_t i = 0; i < note_count; ++i) {
    p.revalidation_notes.push_back(in.str());
  }
  const std::uint32_t stage_history = in.u32();
  if (in.bad() || stage_history > limits.max_stage_history) {
    return false;
  }
  for (std::uint32_t i = 0; i < stage_history; ++i) {
    StageRecord r{};
    r.index = static_cast<std::size_t>(in.u64());
    r.kind = static_cast<StageKind>(in.u8());
    r.generation = StageGeneration(in.u64());
    r.attempt = AttemptId(in.u64());
    r.entered_tick = in.u64();
    r.exited_tick = in.u64();
    r.outcome = static_cast<StageOutcome>(in.u8());
    r.attempts = in.u32();
    r.evidence_generation = EvidenceGeneration(in.u64());
    r.authority_generation = AuthorityGeneration(in.u64());
    r.epoch = CoordinatorEpoch(in.u64());
    if (!read_vector(in, limits.max_resources_per_plan, r.requested)) {
      return false;
    }
    if (!read_vector(in, limits.max_resources_per_plan, r.authorized)) {
      return false;
    }
    if (!read_vector(in, limits.max_resources_per_plan, r.observed)) {
      return false;
    }
    r.last_reason = static_cast<RejectReason>(in.u8());
    if (!is_valid(r.kind) || !is_valid(r.outcome)) {
      return false;
    }
    p.stage_history.push_back(std::move(r));
  }
  const std::uint32_t rollback_history = in.u32();
  if (in.bad() || rollback_history > limits.max_rollback_history) {
    return false;
  }
  for (std::uint32_t i = 0; i < rollback_history; ++i) {
    RollbackRecord r{};
    r.id = RollbackId(in.u64());
    r.generation = RollbackGeneration(in.u64());
    r.epoch = CoordinatorEpoch(in.u64());
    r.kind = static_cast<RollbackKind>(in.u8());
    r.trigger = static_cast<RejectReason>(in.u8());
    r.tick = in.u64();
    if (!read_vector(in, limits.max_resources_per_plan, r.observed_current)) {
      return false;
    }
    if (!read_vector(in, limits.max_resources_per_plan, r.target)) {
      return false;
    }
    if (!read_vector(in, limits.max_resources_per_plan, r.compensation)) {
      return false;
    }
    const std::uint32_t unobserved = in.u32();
    if (in.bad() || unobserved > limits.max_resources_per_plan) {
      return false;
    }
    for (std::uint32_t j = 0; j < unobserved; ++j) {
      r.unobserved.push_back(ResourceId(in.u64()));
    }
    r.assumed_steps = in.u32();
    r.compensation_steps = in.u32();
    r.observed_steps = in.u32();
    r.complete = in.u8() != 0u;
    r.reason = in.str();
    if (!is_valid(r.kind)) {
      return false;
    }
    p.rollback_history.push_back(std::move(r));
  }
  const std::uint32_t completions = in.u32();
  if (in.bad() || completions > limits.max_completion_history) {
    return false;
  }
  for (std::uint32_t i = 0; i < completions; ++i) {
    CompletionRecord r{};
    r.id = CompletionId(in.u64());
    r.generation = CompletionGeneration(in.u64());
    r.epoch = CoordinatorEpoch(in.u64());
    r.stage_index = static_cast<std::size_t>(in.u64());
    r.stage_generation = StageGeneration(in.u64());
    r.attempt = AttemptId(in.u64());
    r.evidence_generation = EvidenceGeneration(in.u64());
    r.state = static_cast<CompletionState>(in.u8());
    r.recorded_tick = in.u64();
    r.idempotency_key = in.u64();
    if (!read_vector(in, limits.max_resources_per_plan, r.applied)) {
      return false;
    }
    r.reject = static_cast<RejectReason>(in.u8());
    r.replayed = in.u8() != 0u;
    if (!is_valid(r.state)) {
      return false;
    }
    p.completions.push_back(std::move(r));
  }
  const std::uint32_t transitions = in.u32();
  if (in.bad() || transitions > 4096u) {
    return false;
  }
  for (std::uint32_t i = 0; i < transitions; ++i) {
    p.transition_ticks.push_back(in.u64());
  }
  if (!read_vector(in, limits.max_resources_per_plan, p.last_observed)) {
    return false;
  }
  if (!is_valid(p.state) || (p.last_reason != RejectReason::NONE && !is_valid(p.last_reason))) {
    return false;
  }
  return !in.bad();
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

bool serialize_state(const DurableState& state, const EngineLimits& limits, std::vector<std::uint8_t>& out,
                     std::string& err) {
  err.clear();
  out.clear();
  if (!limits.valid()) {
    return set_err(err, "engine limits invalid");
  }
  if (state.policies.size() > static_cast<std::size_t>(limits.max_policies) ||
      state.plans.size() > static_cast<std::size_t>(limits.max_plans)) {
    return set_err(err, "durable state exceeds configured limits");
  }

  Out body;
  body.u64(state.epoch.value());
  body.u64(state.snapshot_tick);
  body.u64(state.journal_sequence);
  body.u32(static_cast<std::uint32_t>(state.policies.size()));
  for (const RecoveryPolicy& policy : state.policies) {
    write_policy(body, policy);
  }
  body.u32(static_cast<std::uint32_t>(state.plans.size()));
  for (const RecoveryPlan& plan : state.plans) {
    write_plan(body, plan);
  }
  if (body.buffer().size() > kSnapshotMaxBytes) {
    return set_err(err, "serialized state exceeds the maximum snapshot size");
  }

  Out header;
  header.u32(kSnapshotMagic);
  header.u32(kSnapshotFormatVersion);
  header.u64(static_cast<std::uint64_t>(body.buffer().size()));

  out.reserve(header.buffer().size() + body.buffer().size() + 4u);
  out.insert(out.end(), header.buffer().begin(), header.buffer().end());
  out.insert(out.end(), body.buffer().begin(), body.buffer().end());
  const std::uint32_t checksum = crc32(out.data(), out.size());
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((checksum >> (8 * i)) & 0xffu));
  }
  return true;
}

bool deserialize_state(const std::uint8_t* data, std::size_t size, const EngineLimits& limits,
                       DurableState& out, std::string& err) {
  err.clear();
  out = DurableState{};
  if (!limits.valid()) {
    return set_err(err, "engine limits invalid");
  }
  if (data == nullptr) {
    return set_err(err, "null snapshot buffer");
  }
  if (size < kSnapshotHeaderBytes + 4u) {
    return set_err(err, "snapshot is truncated");
  }
  if (size > kSnapshotMaxBytes) {
    return set_err(err, "snapshot exceeds the maximum size");
  }
  In header(data, kSnapshotHeaderBytes);
  if (header.u32() != kSnapshotMagic) {
    return set_err(err, "snapshot magic mismatch");
  }
  const std::uint32_t version = header.u32();
  if (version != kSnapshotFormatVersion) {
    return set_err(err, "unsupported snapshot version");
  }
  const std::uint64_t body_size = header.u64();
  if (header.bad() || body_size != static_cast<std::uint64_t>(size - kSnapshotHeaderBytes - 4u)) {
    return set_err(err, "snapshot length does not match its header");
  }
  const std::uint32_t stored_crc = [&]() {
    std::uint32_t v = 0;
    const std::size_t base = size - 4u;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data[base + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
  }();
  if (crc32(data, size - 4u) != stored_crc) {
    return set_err(err, "snapshot checksum mismatch");
  }

  In in(data + kSnapshotHeaderBytes, static_cast<std::size_t>(body_size));
  DurableState state{};
  state.format_version = version;
  state.epoch = CoordinatorEpoch(in.u64());
  state.snapshot_tick = in.u64();
  state.journal_sequence = in.u64();
  if (!state.epoch.is_valid()) {
    return set_err(err, "snapshot epoch is invalid");
  }
  const std::uint32_t policy_count = in.u32();
  if (in.bad() || policy_count > limits.max_policies) {
    return set_err(err, "snapshot policy count out of range");
  }
  for (std::uint32_t i = 0; i < policy_count; ++i) {
    RecoveryPolicy policy{};
    if (!read_policy(in, limits.max_stages_per_policy, limits.max_requirements_per_stage, policy)) {
      return set_err(err, "snapshot contains an invalid policy");
    }
    std::string verr;
    if (!validate_policy(policy, limits, verr)) {
      return set_err(err, "snapshot policy failed validation");
    }
    state.policies.push_back(std::move(policy));
  }
  const std::uint32_t plan_count = in.u32();
  if (in.bad() || plan_count > limits.max_plans) {
    return set_err(err, "snapshot plan count out of range");
  }
  for (std::uint32_t i = 0; i < plan_count; ++i) {
    RecoveryPlan plan{};
    if (!read_plan(in, limits, plan)) {
      return set_err(err, "snapshot contains an invalid plan");
    }
    state.plans.push_back(std::move(plan));
  }
  if (in.bad()) {
    return set_err(err, "snapshot body is malformed");
  }
  if (in.remaining() != 0u) {
    return set_err(err, "snapshot contains trailing bytes");
  }
  for (const RecoveryPlan& plan : state.plans) {
    if (!plan.constrained.structurally_valid() || !plan.rollback_target.structurally_valid() ||
        !plan.baseline.structurally_valid() || !plan.authorized.structurally_valid() ||
        !plan.last_observed.structurally_valid()) {
      return set_err(err, "snapshot plan contains a structurally invalid restoration vector");
    }
    bool found = false;
    for (const RecoveryPolicy& policy : state.policies) {
      if (policy.id == plan.policy_id) {
        found = true;
        break;
      }
    }
    if (!found) {
      return set_err(err, "snapshot plan references an absent policy");
    }
  }
  out = std::move(state);
  return true;
}

bool serialize_journal_record(const JournalRecord& record, std::vector<std::uint8_t>& out, std::string& err) {
  err.clear();
  out.clear();
  if (!record.epoch.is_valid()) {
    return set_err(err, "journal record epoch is invalid");
  }
  if (record.payload.size() > kJournalMaxRecordBytes) {
    return set_err(err, "journal record payload exceeds the maximum size");
  }
  Out body;
  body.u64(record.sequence);
  body.u64(record.tick);
  body.u64(record.epoch.value());
  body.str(record.kind);
  body.bytes(record.payload);
  if (body.buffer().size() > kJournalMaxRecordBytes) {
    return set_err(err, "journal record exceeds the maximum size");
  }
  Out frame;
  frame.u32(kJournalMagic);
  frame.u32(static_cast<std::uint32_t>(body.buffer().size()));
  out.reserve(frame.buffer().size() + body.buffer().size() + 4u);
  out.insert(out.end(), frame.buffer().begin(), frame.buffer().end());
  out.insert(out.end(), body.buffer().begin(), body.buffer().end());
  const std::uint32_t checksum = crc32(out.data(), out.size());
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((checksum >> (8 * i)) & 0xffu));
  }
  return true;
}

bool replay_journal(const std::uint8_t* data, std::size_t size, CoordinatorEpoch live_epoch,
                    const EngineLimits& limits, std::vector<JournalRecord>& out, RestoreReport& report,
                    std::string& err) {
  err.clear();
  out.clear();
  if (!limits.valid()) {
    return set_err(err, "engine limits invalid");
  }
  if (data == nullptr && size != 0u) {
    return set_err(err, "null journal buffer");
  }
  if (size > kJournalMaxBytes) {
    return set_err(err, "journal exceeds the maximum size");
  }
  std::size_t offset = 0;
  std::uint64_t last_sequence = 0;
  while (offset < size) {
    const std::size_t remaining = size - offset;
    if (remaining < kJournalHeaderBytes + 4u) {
      report.journal_truncated_bytes += remaining;  // crash between records
      break;
    }
    In header(data + offset, kJournalHeaderBytes);
    if (header.u32() != kJournalMagic) {
      report.journal_records_rejected += 1;
      err = "journal magic mismatch";
      return true;
    }
    const std::uint32_t body_size = header.u32();
    if (header.bad() || body_size > kJournalMaxRecordBytes) {
      report.journal_records_rejected += 1;
      err = "journal record length out of range";
      return true;
    }
    if (remaining < kJournalHeaderBytes + static_cast<std::size_t>(body_size) + 4u) {
      report.journal_truncated_bytes += remaining;
      break;
    }
    const std::size_t frame_size = kJournalHeaderBytes + static_cast<std::size_t>(body_size) + 4u;
    std::uint32_t stored_crc = 0;
    for (int i = 0; i < 4; ++i) {
      stored_crc |= static_cast<std::uint32_t>(data[offset + frame_size - 4u + static_cast<std::size_t>(i)])
                    << (8 * i);
    }
    if (crc32(data + offset, frame_size - 4u) != stored_crc) {
      report.journal_records_rejected += 1;
      err = "journal record checksum mismatch";
      return true;
    }
    In in(data + offset + kJournalHeaderBytes, body_size);
    JournalRecord record{};
    record.sequence = in.u64();
    record.tick = in.u64();
    record.epoch = CoordinatorEpoch(in.u64());
    record.kind = in.str();
    record.payload = in.bytes();
    if (in.bad() || in.remaining() != 0u) {
      report.journal_records_rejected += 1;
      err = "journal record body is malformed";
      return true;
    }
    if (!record.epoch.is_valid()) {
      report.journal_records_rejected += 1;
      offset += frame_size;
      continue;
    }
    if (record.sequence <= last_sequence) {
      report.journal_records_rejected += 1;  // sequence regression: replay stops
      err = "journal sequence regression";
      return true;
    }
    last_sequence = record.sequence;
    if (live_epoch.is_valid() && record.epoch < live_epoch) {
      report.journal_records_rejected += 1;  // stale epoch: never replayed
      offset += frame_size;
      continue;
    }
    report.journal_records_replayed += 1;
    out.push_back(std::move(record));
    offset += frame_size;
  }
  return true;
}

}  // namespace congestion_recovery
