// Congestion Recovery - durability, corruption and restart suite.
// Copyright 2026 Summon Software Labs.
#include <string>
#include <vector>

#include "support.hpp"

using namespace congestion_recovery;
using namespace crtest;

namespace {

void advance_to(Fixture& fx, std::size_t last_stage, std::uint64_t& tick) {
  for (std::size_t i = 0; i < last_stage; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    CHK(step(fx, tick, rate_for(i), true, &applied).advanced);
  }
}

std::vector<std::uint8_t> serialize(const DurableState& state) {
  std::vector<std::uint8_t> bytes;
  std::string err;
  CHK(serialize_state(state, EngineLimits{}, bytes, err));
  CHK(err.empty());
  return bytes;
}

DurableState round_trip(const DurableState& state) {
  const std::vector<std::uint8_t> bytes = serialize(state);
  DurableState parsed{};
  std::string err;
  CHK(deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  return parsed;
}

void test_round_trip_and_restart_semantics() {
  SECTION("snapshot round trip and conservative restart");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 3, tick);

  const DurableState state = fx.engine.export_state();
  CHK(state.plans.size() == 1);
  CHK(state.policies.size() == 1);
  CHK(state.epoch == CoordinatorEpoch(1));

  const DurableState parsed = round_trip(state);
  CHK(parsed.plans.size() == 1);
  CHK(parsed.epoch == CoordinatorEpoch(1));
  if (parsed.plans.empty()) {
    return;
  }
  CHK(parsed.plans.front().stage_index == 3);
  CHK(parsed.plans.front().stage_history.size() >= 3);
  CHK(parsed.plans.front().state == RecoveryState::HOLDING);

  // Restart requires an epoch advance.
  RecoveryEngine engine{EngineLimits{}};
  const RestoreReport refused = engine.import_state(parsed, CoordinatorEpoch(1));
  CHK(!refused.valid);
  const RestoreReport report = engine.import_state(parsed, CoordinatorEpoch(2));
  CHK(report.valid && report.loaded);
  CHK(report.epoch_before == 1 && report.epoch_after == 2);
  CHK(report.plans_loaded == 1);
  CHK(report.plans_requiring_revalidation == 1);
  CHK(report.authorities_dropped == 1);

  const RecoveryPlan* restored = engine.plan(RecoveryPlanId(1));
  CHK(restored != nullptr);
  CHK(restored->state == RecoveryState::REVALIDATION_REQUIRED);
  CHK(restored->revalidation_required);
  CHK(restored->epoch == CoordinatorEpoch(2));
  CHK(restored->last_observed.empty());
  // Durable lineage survives.
  CHK(restored->stage_history.size() >= 3);
  CHK(restored->stage_index == 3);

  // Live authority never comes back from disk.
  CHK(engine.authority_snapshot().empty());
  const EvidenceSnapshot snapshot = fx.snapshot(tick + 100, 5000, true, nullptr);
  const StageDecision d = engine.advance(RecoveryPlanId(1), restored->generation, snapshot);
  CHK(d.decision == RecoveryDecision::REVALIDATE);
  CHK(d.reason == RejectReason::REVALIDATION_BOUNDARY);
}

void test_completion_survives_restart() {
  SECTION("committed completion survives restart");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);
  const RestorationVector applied = target_for(4);
  tick += 20;
  CHK(step(fx, tick, rate_for(4), true, &applied).completed);

  const DurableState state = fx.engine.export_state();
  const DurableState parsed = round_trip(state);
  CHK(parsed.plans.size() == 1);
  if (parsed.plans.empty()) {
    return;
  }
  CHK(parsed.plans.front().completions.size() == 1);
  CHK(parsed.plans.front().completions.front().state == CompletionState::COMMITTED);

  RecoveryEngine engine{EngineLimits{}};
  const RestoreReport report = engine.import_state(parsed, CoordinatorEpoch(2));
  CHK(report.valid);
  CHK(report.completions_preserved == 1);
  CHK(report.plans_requiring_revalidation == 0);
  const RecoveryPlan* restored = engine.plan(RecoveryPlanId(1));
  CHK(restored != nullptr && restored->state == RecoveryState::COMPLETED);
  CHK(restored != nullptr && restored->completion_count == 1);
}

void test_rollback_history_survives_restart() {
  SECTION("rollback history survives restart");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);
  RollbackRequest rollback{};
  rollback.plan = fx.handle.id;
  rollback.plan_generation = fx.engine.plan(fx.handle.id)->generation;
  rollback.epoch = fx.epoch;
  rollback.observed_current = make_observed(400, 500);
  rollback.provenance = Provenance::MEASURED;
  rollback.now_tick = tick + 20;
  rollback.reason = "recurrence";
  CHK(fx.engine.rollback(rollback).ok);

  const DurableState parsed = round_trip(fx.engine.export_state());
  CHK(parsed.plans.size() == 1);
  if (parsed.plans.empty()) {
    return;
  }
  CHK(parsed.plans.front().rollback_history.size() == 1);
  CHK(parsed.plans.front().rollback_history.front().compensation_steps == 2);
  CHK(parsed.plans.front().rollback_count == 1);

  RecoveryEngine engine{EngineLimits{}};
  const RestoreReport report = engine.import_state(parsed, CoordinatorEpoch(2));
  CHK(report.valid && report.rollbacks_preserved == 1);
  const RecoveryPlan* restored = engine.plan(RecoveryPlanId(1));
  CHK(restored != nullptr && restored->rollback_history.size() == 1);
}

void test_corruption_rejection() {
  SECTION("corrupt snapshots are rejected");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);
  const std::vector<std::uint8_t> good = serialize(fx.engine.export_state());
  CHK(!good.empty());

  DurableState parsed{};
  std::string err;

  // Truncation at every prefix length must never yield a valid state.
  for (std::size_t cut = 0; cut < good.size(); cut += 7u) {
    DurableState partial{};
    std::string partial_err;
    CHK(!deserialize_state(good.data(), cut, EngineLimits{}, partial, partial_err));
  }

  // Bad magic.
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[0] ^= 0xffu;
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
    CHK(err.find("magic") != std::string::npos);
  }
  // Unsupported version.
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[4] = 0x7fu;
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
    CHK(err.find("version") != std::string::npos);
  }
  // Checksum mismatch.
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[bytes.size() / 2u] ^= 0x01u;
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  }
  // Trailing bytes.
  {
    std::vector<std::uint8_t> bytes = good;
    bytes.push_back(0u);
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  }
  // Declared body length does not match reality.
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[8] = static_cast<std::uint8_t>(bytes[8] + 1u);
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  }
  // Null and empty buffers.
  {
    CHK(!deserialize_state(nullptr, 0u, EngineLimits{}, parsed, err));
  }
  // A plan referencing a policy that is not present is rejected.
  {
    DurableState broken = fx.engine.export_state();
    broken.plans.front().policy_id = PolicyId(9999);
    std::vector<std::uint8_t> bytes;
    CHK(serialize_state(broken, EngineLimits{}, bytes, err));
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  }
  // A policy that fails validation is rejected before it can be imported.
  {
    DurableState broken = fx.engine.export_state();
    broken.policies.front().stages.pop_back();
    std::vector<std::uint8_t> bytes;
    CHK(serialize_state(broken, EngineLimits{}, bytes, err));
    CHK(!deserialize_state(bytes.data(), bytes.size(), EngineLimits{}, parsed, err));
  }
  // Limits smaller than the payload are refused.
  {
    EngineLimits tiny{};
    tiny.max_plans = 1;
    DurableState one{};
    tiny.max_policies = 1;
    CHK(deserialize_state(good.data(), good.size(), tiny, one, err));
  }
}

void test_journal_replay() {
  SECTION("journal replay, truncation and stale epochs");
  std::vector<std::uint8_t> stream;
  std::string err;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    JournalRecord record{};
    record.sequence = i;
    record.tick = i * 10u;
    record.epoch = CoordinatorEpoch(1);
    record.kind = (i % 2u == 0u) ? "COMMIT" : "INTENT";
    record.payload = {static_cast<std::uint8_t>(i)};
    std::vector<std::uint8_t> bytes;
    CHK(serialize_journal_record(record, bytes, err));
    stream.insert(stream.end(), bytes.begin(), bytes.end());
  }
  RestoreReport report{};
  std::vector<JournalRecord> replayed;
  CHK(replay_journal(stream.data(), stream.size(), CoordinatorEpoch(1), EngineLimits{}, replayed, report, err));
  CHK(replayed.size() == 4);
  CHK(report.journal_records_replayed == 4);
  CHK(report.journal_records_rejected == 0);

  // A truncated tail is an expected crash artifact, not corruption. The count
  // reports the trailing bytes that belong to an incomplete record.
  RestoreReport tail_report{};
  replayed.clear();
  CHK(replay_journal(stream.data(), stream.size() - 3u, CoordinatorEpoch(1), EngineLimits{}, replayed,
                     tail_report, err));
  CHK(replayed.size() == 3);
  CHK(tail_report.journal_truncated_bytes > 0u);
  CHK(tail_report.journal_records_rejected == 0u);

  // A stream cut exactly on a record boundary loses nothing at all.
  {
    std::vector<std::uint8_t> first_three;
    for (int i = 0; i < 3; ++i) {
      JournalRecord record{};
      record.sequence = static_cast<std::uint64_t>(i + 1);
      record.tick = static_cast<std::uint64_t>(i + 1);
      record.epoch = CoordinatorEpoch(1);
      record.kind = "STAGE";
      std::vector<std::uint8_t> bytes;
      CHK(serialize_journal_record(record, bytes, err));
      first_three.insert(first_three.end(), bytes.begin(), bytes.end());
    }
    RestoreReport clean_report{};
    replayed.clear();
    CHK(replay_journal(first_three.data(), first_three.size(), CoordinatorEpoch(1), EngineLimits{}, replayed,
                       clean_report, err));
    CHK(replayed.size() == 3);
    CHK(clean_report.journal_truncated_bytes == 0u);
    CHK(clean_report.journal_records_rejected == 0u);
  }

  // Corruption inside a complete record is a rejection.
  {
    std::vector<std::uint8_t> broken = stream;
    broken[broken.size() - 1u] ^= 0xffu;
    RestoreReport corrupt_report{};
    replayed.clear();
    CHK(replay_journal(broken.data(), broken.size(), CoordinatorEpoch(1), EngineLimits{}, replayed,
                       corrupt_report, err));
    CHK(corrupt_report.journal_records_rejected >= 1);
  }

  // Records from an older epoch are never replayed.
  {
    std::vector<std::uint8_t> old;
    JournalRecord record{};
    record.sequence = 1;
    record.tick = 1;
    record.epoch = CoordinatorEpoch(1);
    record.kind = "INTENT";
    std::vector<std::uint8_t> bytes;
    CHK(serialize_journal_record(record, bytes, err));
    old.insert(old.end(), bytes.begin(), bytes.end());
    RestoreReport old_report{};
    replayed.clear();
    CHK(replay_journal(old.data(), old.size(), CoordinatorEpoch(2), EngineLimits{}, replayed, old_report, err));
    CHK(replayed.empty());
    CHK(old_report.journal_records_rejected == 1);
  }

  // A sequence regression stops replay rather than reordering history.
  {
    std::vector<std::uint8_t> regressed;
    for (std::uint64_t i = 2; i >= 1; --i) {
      JournalRecord record{};
      record.sequence = i;
      record.tick = i;
      record.epoch = CoordinatorEpoch(1);
      record.kind = "STAGE";
      std::vector<std::uint8_t> bytes;
      CHK(serialize_journal_record(record, bytes, err));
      regressed.insert(regressed.end(), bytes.begin(), bytes.end());
      if (i == 1) {
        break;
      }
    }
    RestoreReport regressed_report{};
    replayed.clear();
    CHK(replay_journal(regressed.data(), regressed.size(), CoordinatorEpoch(1), EngineLimits{}, replayed,
                       regressed_report, err));
    CHK(replayed.size() == 1);
    CHK(regressed_report.journal_records_rejected == 1);
  }

  // A record whose body is malformed is rejected.
  {
    JournalRecord record{};
    record.sequence = 0;  // invalid epoch below
    record.epoch = CoordinatorEpoch(0);
    std::vector<std::uint8_t> bytes;
    CHK(!serialize_journal_record(record, bytes, err));
  }
}

void test_export_import_is_deterministic() {
  SECTION("export/import is deterministic");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 2, tick);
  const DurableState first = fx.engine.export_state();
  const DurableState second = fx.engine.export_state();
  const std::vector<std::uint8_t> a = serialize(first);
  const std::vector<std::uint8_t> b = serialize(second);
  CHK(a == b);
  CHK(crc32(a.data(), a.size()) == crc32(b.data(), b.size()));
}

}  // namespace

int main() {
  crtest::init();
  test_round_trip_and_restart_semantics();
  test_completion_survives_restart();
  test_rollback_history_survives_restart();
  test_corruption_rejection();
  test_journal_replay();
  test_export_import_is_deterministic();
  return summary("cr_persistence");
}
