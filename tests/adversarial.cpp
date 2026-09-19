// Congestion Recovery - adversarial input suite.
// Copyright 2026 Summon Software Labs.
#include <cmath>
#include <string>
#include <vector>

#include "congestion_recovery/protocol.hpp"
#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

void test_frame_adversarial() {
  SECTION("frame decoding rejects hostile input");
  Frame frame{};
  frame.kind = MessageKind::STAGE_REPORT;
  frame.epoch = CoordinatorEpoch(3);
  frame.worker = WorkerId(2);
  frame.boot = WorkerBoot(4);
  frame.sequence = 9;
  frame.body = {1u, 2u, 3u, 4u};
  std::vector<std::uint8_t> bytes;
  std::string err;
  CHK(encode_frame(frame, bytes, err));
  CHK(bytes.size() > 24u);

  Frame decoded{};
  std::size_t consumed = 0;
  CHK(decode_frame(bytes.data(), bytes.size(), decoded, consumed, err));
  CHK(consumed == bytes.size());
  CHK(decoded.kind == MessageKind::STAGE_REPORT);
  CHK(decoded.epoch == CoordinatorEpoch(3));
  CHK(decoded.worker == WorkerId(2));
  CHK(decoded.boot == WorkerBoot(4));
  CHK(decoded.sequence == 9);
  CHK(decoded.body == frame.body);

  // Every truncation is reported as truncated, never accepted.
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Frame partial{};
    std::size_t used = 0;
    std::string partial_err;
    CHK(!decode_frame(bytes.data(), cut, partial, used, partial_err));
    if (cut < 8u || partial_err != "truncated") {
      CHK(partial_err == "truncated" || partial_err == "oversized frame");
    }
  }
  // Bad magic.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[0] = 0x00u;
    Frame out{};
    CHK(!decode_frame(bad.data(), bad.size(), out, consumed, err));
    CHK(err.find("magic") != std::string::npos);
  }
  // Checksum mismatch.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[bad.size() - 1u] ^= 0xffu;
    Frame out{};
    CHK(!decode_frame(bad.data(), bad.size(), out, consumed, err));
    CHK(err.find("checksum") != std::string::npos);
  }
  // Oversized declared length.
  {
    std::vector<std::uint8_t> bad = bytes;
    bad[4] = 0xffu;
    bad[5] = 0xffu;
    bad[6] = 0xffu;
    bad[7] = 0x7fu;
    Frame out{};
    CHK(!decode_frame(bad.data(), bad.size(), out, consumed, err));
  }
  // Unknown message kind.
  {
    Frame unknown{};
    unknown.kind = MessageKind::UNKNOWN;
    std::vector<std::uint8_t> out_bytes;
    CHK(!encode_frame(unknown, out_bytes, err));
  }
  // Oversized body is refused before it is ever written.
  {
    Frame huge{};
    huge.kind = MessageKind::STAGE_REPORT;
    huge.body.assign(kMaxBodyBytes + 1u, 0u);
    std::vector<std::uint8_t> out_bytes;
    CHK(!encode_frame(huge, out_bytes, err));
  }
  // Null buffer never dereferences.
  {
    Frame out{};
    CHK(!decode_frame(nullptr, 64u, out, consumed, err));
  }
}

void test_writer_reader_bounds() {
  SECTION("bounded writer and reader");
  Writer writer;
  writer.put_str(std::string(kMaxStringBytes + 1u, 'x'));
  CHK(writer.bad());
  Writer ok_writer;
  ok_writer.put_u32(7u);
  ok_writer.put_str("abc");
  ok_writer.put_i64(-5);
  ok_writer.put_bool(true);
  ok_writer.put_f64(0.25);
  CHK(!ok_writer.bad());
  Reader reader(ok_writer.buffer().data(), ok_writer.buffer().size());
  CHK(reader.get_u32() == 7u);
  CHK(reader.get_str() == "abc");
  CHK(reader.get_i64() == -5);
  CHK(reader.get_bool());
  CHK(reader.get_f64() == 0.25);
  CHK(!reader.bad());
  CHK(reader.get_u32() == 0u);
  CHK(reader.bad());
}

void test_engine_hostile_input() {
  SECTION("engine rejects hostile evidence and identifiers");
  Fixture fx;
  CHK(fx.build());
  const RestorationVector applied = target_for(0);

  // NaN and out-of-range confidence never satisfy a requirement.
  EvidenceSnapshot nan_snapshot = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : nan_snapshot.entries) {
    e.confidence = std::nan("");
  }
  StageDecision d = fx.engine.advance(fx.handle.id, fx.handle.generation, nan_snapshot);
  CHK(!d.advanced && d.decision == RecoveryDecision::REJECT_EVIDENCE);
  EvidenceSnapshot high = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : high.entries) {
    e.confidence = 4.0;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, high);
  CHK(!d.advanced);

  // Zero sample counts are not evidence.
  EvidenceSnapshot zero = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : zero.entries) {
    e.sample_count = 0u;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, zero);
  CHK(!d.advanced && d.decision == RecoveryDecision::REJECT_EVIDENCE);

  // An impossible duration is refused.
  EvidenceSnapshot absurd = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : absurd.entries) {
    e.duration_ticks = 1000000u;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, absurd);
  CHK(!d.advanced);

  // An unknown evidence kind cannot satisfy anything.
  EvidenceSnapshot unknown = fx.snapshot(1020, rate_for(0), true, &applied);
  for (Evidence& e : unknown.entries) {
    e.kind = EvidenceKind::UNKNOWN;
  }
  d = fx.engine.advance(fx.handle.id, fx.handle.generation, unknown);
  CHK(!d.advanced && d.decision == RecoveryDecision::REJECT_EVIDENCE);

  // A plan that does not exist is never advanced.
  d = fx.engine.advance(RecoveryPlanId(0), PlanGeneration(1), unknown);
  CHK(!d.advanced && d.reason == RejectReason::NOT_FOUND);
}

void test_limits_and_identifier_hygiene() {
  SECTION("limits and identifier hygiene");
  EngineLimits limits{};
  limits.max_plans = 1;
  limits.max_policies = 1;
  RecoveryEngine engine{limits};
  engine.begin_epoch(CoordinatorEpoch(1));
  const RecoveryPolicy policy = make_policy();
  const PolicyId policy_id = engine.define_policy(policy);
  CHK(policy_id.is_valid());
  CHK(!engine.define_policy(make_policy()).is_valid());  // policy limit
  const RecoveryPolicy* stored = engine.policy(policy_id);

  CreatePlanRequest request{};
  request.policy_id = policy_id;
  request.policy_generation = stored->generation;
  request.congestion = CongestionRefId(1);
  request.intervention = InterventionId(1);
  request.epoch = engine.epoch();
  request.now_tick = 1000;
  request.resources.push_back(ResourceBinding{kR1, ResourceGeneration(1)});
  request.resources.push_back(ResourceBinding{kR2, ResourceGeneration(1)});
  request.baseline = make_levels(kBaselineR1, kBaselineR2);
  request.baseline_known = true;
  request.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  request.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);
  const PlanHandle handle = engine.create_plan(request);
  CHK(handle.is_valid());
  CHK(!engine.create_plan(request).is_valid());  // plan limit

  // Live plans are never silently discarded.
  CHK(!engine.erase_plan(handle.id));
  CHK(engine.has_plan(handle.id));
  CHK(!engine.has_plan(RecoveryPlanId(4242)));

  // Invalid identifiers are refused rather than defaulted.
  CHK(!engine.grant_authority(RecoveryAuthority{}));
  RecoveryAuthority bad = RecoveryAuthority{};
  bad.id = AuthorityId(1);
  bad.generation = AuthorityGeneration(1);
  bad.epoch = engine.epoch();
  bad.resource = kR1;
  bad.unit = RestorationUnit::UNKNOWN;
  bad.ceiling = 10;
  CHK(!engine.grant_authority(bad));
  bad.unit = RestorationUnit::BYTES_PER_SECOND;
  bad.ceiling = 5;
  bad.floor = 10;  // ceiling below floor
  CHK(!engine.grant_authority(bad));
  bad.floor = 0;
  bad.expires_at_tick = 1;
  bad.granted_at_tick = 100;  // expiry before grant
  CHK(!engine.grant_authority(bad));
  CHK(engine.notify_resource_change(ResourceId{}, ResourceGeneration(1)) == 0);
  CHK(engine.notify_policy_change(PolicyId{}, PolicyGeneration(1)) == 0);
}

void test_invalid_limits_fall_back() {
  SECTION("invalid limits fall back to safe defaults");
  EngineLimits bad{};
  bad.max_plans = 0;
  RecoveryEngine engine{bad};
  CHK(engine.limits().max_plans > 0u);
  EngineLimits absurd{};
  absurd.max_plans = 1000000u;
  RecoveryEngine other{absurd};
  CHK(other.limits().max_plans <= 65536u);
}

void test_plan_creation_rejects_contradictions() {
  SECTION("plan creation rejects contradictory vectors");
  Fixture fx;
  CHK(fx.build());

  CreatePlanRequest request{};
  request.policy_id = fx.policy;
  request.policy_generation = fx.policy_generation;
  request.congestion = CongestionRefId(1);
  request.intervention = InterventionId(1);
  request.epoch = fx.epoch;
  request.now_tick = 1000;
  request.resources.push_back(ResourceBinding{kR1, ResourceGeneration(1)});
  request.resources.push_back(ResourceBinding{kR2, ResourceGeneration(1)});
  request.baseline = make_levels(kBaselineR1, kBaselineR2);
  request.baseline_known = true;
  request.constrained = make_levels(kConstrainedR1, kConstrainedR2);
  request.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);

  // Invalid congestion/intervention references.
  CreatePlanRequest no_refs = request;
  no_refs.congestion = CongestionRefId{};
  CHK(!fx.engine.create_plan(no_refs).is_valid());
  CreatePlanRequest no_intervention = request;
  no_intervention.intervention = InterventionId{};
  CHK(!fx.engine.create_plan(no_intervention).is_valid());

  // Resource generation must be present.
  CreatePlanRequest no_generation = request;
  no_generation.resources.clear();
  no_generation.resources.push_back(ResourceBinding{kR1, ResourceGeneration{}});
  no_generation.resources.push_back(ResourceBinding{kR2, ResourceGeneration(1)});
  CHK(!fx.engine.create_plan(no_generation).is_valid());

  // A baseline that omits a bound resource is inconsistent.
  CreatePlanRequest short_baseline = request;
  short_baseline.baseline = make_levels(kBaselineR1, kBaselineR1);
  short_baseline.baseline = RestorationVector{};
  (void)short_baseline.baseline.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, kBaselineR1);
  CHK(!fx.engine.create_plan(short_baseline).is_valid());

  // A unit mismatch between the constrained state and the ladder is refused.
  CreatePlanRequest unit_mismatch = request;
  unit_mismatch.constrained = RestorationVector{};
  (void)unit_mismatch.constrained.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, kConstrainedR1);
  (void)unit_mismatch.constrained.set_level(kR2, RestorationUnit::FLOWS, kConstrainedR2);
  CHK(!fx.engine.create_plan(unit_mismatch).is_valid());

  // A rollback target above the constrained envelope is refused.
  CreatePlanRequest upward = request;
  upward.rollback_target = make_levels(1000, 1000);
  CHK(!fx.engine.create_plan(upward).is_valid());
}

void test_revalidate_rejects_mismatches() {
  SECTION("revalidation rejects mismatches");
  Fixture fx;
  CHK(fx.build());
  CHK(fx.engine.notify_resource_change(kR1, ResourceGeneration(9)) == 1);
  std::vector<ResourceBinding> wrong_size;
  wrong_size.push_back(ResourceBinding{kR1, ResourceGeneration(9)});
  RevalidationOutcome outcome =
      fx.engine.revalidate(fx.handle.id, fx.handle.generation, fx.policy_generation, wrong_size, 1030);
  CHK(!outcome.ok && outcome.reason == RejectReason::STALE_RESOURCE);

  std::vector<ResourceBinding> wrong_policy;
  wrong_policy.push_back(ResourceBinding{kR1, ResourceGeneration(9)});
  wrong_policy.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
  outcome = fx.engine.revalidate(fx.handle.id, fx.handle.generation, PolicyGeneration(9), wrong_policy, 1030);
  CHK(!outcome.ok && outcome.reason == RejectReason::STALE_POLICY);

  outcome = fx.engine.revalidate(fx.handle.id, PlanGeneration(9), fx.policy_generation, wrong_policy, 1030);
  CHK(!outcome.ok && outcome.reason == RejectReason::GENERATION_MISMATCH);

  outcome = fx.engine.revalidate(RecoveryPlanId(777), PlanGeneration(1), fx.policy_generation, wrong_policy,
                                 1030);
  CHK(!outcome.ok && outcome.reason == RejectReason::NOT_FOUND);
}

void test_adjacent_boundary_is_not_absorbed() {
  SECTION("boundary: requests are emitted, never executed");
  Fixture fx;
  CHK(fx.build());
  // The engine exposes no API that detects congestion, admits traffic, computes
  // paths or enforces rates. It records bounded requests for the owning runtime.
  AdjacentRequestLog log(2);
  AdjacentRequest request{};
  request.kind = AdjacentRequestKind::REQUEST_PATH_REVALIDATION;
  request.plan = fx.handle.id;
  request.owner_runtime = "Path Authority";
  request.detail = "recovery observed a path change";
  CHK(log.push(request));
  CHK(log.size() == 1);
  CHK(log.entries().front().owner_runtime == "Path Authority");
  CHK(fx.engine.adjacent_requests().size() == 0u);
  fx.engine.clear_adjacent_requests();
  CHK(fx.engine.adjacent_requests().size() == 0u);
}

}  // namespace

int main() {
  crtest::init();
  test_frame_adversarial();
  test_writer_reader_bounds();
  test_engine_hostile_input();
  test_limits_and_identifier_hygiene();
  test_invalid_limits_fall_back();
  test_plan_creation_rejects_contradictions();
  test_revalidate_rejects_mismatches();
  test_adjacent_boundary_is_not_absorbed();
  return summary("cr_adversarial");
}
