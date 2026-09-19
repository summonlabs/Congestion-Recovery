// Congestion Recovery - concurrency and race suite.
// Copyright 2026 Summon Software Labs.
//
// The engine holds exactly one mutex, never calls a callback or performs I/O
// while holding it, and owns no threads. These tests exercise every public
// entry point from many threads at once and assert that authoritative state
// stays linearizable and monotonically advancing.
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
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

void test_concurrent_evaluation_is_deterministic() {
  SECTION("concurrent pure evaluation is deterministic");
  Fixture fx;
  CHK(fx.build());
  const RestorationVector applied = target_for(0);
  const EvidenceSnapshot snapshot = fx.snapshot(1020, rate_for(0), true, &applied);

  constexpr int kThreads = 8;
  std::vector<StageDecision> results(static_cast<std::size_t>(kThreads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(kThreads));
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&, i]() {
      for (int repeat = 0; repeat < 200; ++repeat) {
        results[static_cast<std::size_t>(i)] =
            fx.engine.evaluate(fx.handle.id, fx.handle.generation, snapshot);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  for (int i = 1; i < kThreads; ++i) {
    CHK(results[static_cast<std::size_t>(i)].decision == results[0].decision);
    CHK(results[static_cast<std::size_t>(i)].reason == results[0].reason);
    CHK(results[static_cast<std::size_t>(i)].stage_index_before == results[0].stage_index_before);
  }
  // Pure evaluation changed nothing.
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  CHK(plan != nullptr && plan->stage_index == 0);
  CHK(plan != nullptr && plan->total_attempts == 1);
  CHK(fx.engine.evaluations() == 0);
}

void test_concurrent_completion_is_single_commit() {
  SECTION("exactly one concurrent completion commits");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1000;
  advance_to(fx, 5, tick);
  const RestorationVector applied = target_for(4);
  const EvidenceSnapshot snapshot = fx.snapshot(tick + 20, rate_for(4), true, &applied);
  const PlanGeneration generation = fx.engine.plan(fx.handle.id)->generation;

  constexpr int kThreads = 8;
  std::atomic<int> committed{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(kThreads));
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      const CompletionOutcome outcome = fx.engine.complete(fx.handle.id, generation, snapshot);
      if (outcome.ok && outcome.duplicate) {
        duplicates.fetch_add(1);
      } else if (outcome.ok) {
        committed.fetch_add(1);
      } else {
        failures.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CHK(committed.load() == 1);
  CHK(duplicates.load() == kThreads - 1);
  CHK(failures.load() == 0);
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  CHK(plan != nullptr && plan->completion_count == 1);
  CHK(plan != nullptr && plan->completions.size() == 1);
  CHK(plan != nullptr && plan->state == RecoveryState::COMPLETED);
}

void test_mixed_concurrent_load() {
  SECTION("mixed concurrent load stays consistent");
  Fixture fx;
  CHK(fx.build());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> explain_ok{0};
  std::atomic<std::uint64_t> reads{0};

  std::thread reader([&]() {
    while (!stop.load()) {
      RecoveryExplanation explanation{};
      if (fx.engine.explain(fx.handle.id, explanation)) {
        explain_ok.fetch_add(1);
      }
      reads.fetch_add(1);
      (void)fx.engine.plan_ids();
      (void)fx.engine.authority_snapshot();
      (void)fx.engine.epoch();
      (void)fx.engine.render_explanation(fx.handle.id);
    }
  });
  std::thread counter([&]() {
    while (!stop.load()) {
      (void)fx.engine.evaluations();
      (void)fx.engine.advances();
      (void)fx.engine.stale_rejections();
    }
  });

  // Wait for the readers to actually enter their loops before mutating, so the
  // interleaving is real rather than an artifact of thread start latency.
  while (reads.load() == 0u) {
    std::this_thread::yield();
  }

  std::uint64_t tick = 1000;
  for (std::size_t i = 0; i < 5; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    const StageDecision d = step(fx, tick, rate_for(i), true, &applied);
    CHK(d.advanced);
  }
  stop.store(true);
  reader.join();
  counter.join();

  CHK(explain_ok.load() > 0u);
  CHK(reads.load() > 0u);
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  CHK(plan != nullptr && plan->stage_index == 5);
  CHK(plan != nullptr && plan->state == RecoveryState::VERIFYING);
  // Stage generations advanced monotonically with no lost update.
  CHK(plan != nullptr && plan->stage_generation.value() >= 6u);
  CHK(plan != nullptr && plan->stage_history.size() >= 5u);
}

void test_concurrent_advance_has_one_winner_per_stage() {
  SECTION("racing advances produce one stage transition per stage");
  Fixture fx;
  CHK(fx.build());
  std::uint64_t tick = 1020;
  const RestorationVector applied = target_for(0);
  const EvidenceSnapshot snapshot = fx.snapshot(tick, rate_for(0), true, &applied);
  const PlanGeneration generation = fx.engine.plan(fx.handle.id)->generation;

  constexpr int kThreads = 8;
  std::atomic<int> advanced{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(kThreads));
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      const StageDecision d = fx.engine.advance(fx.handle.id, generation, snapshot);
      if (d.advanced) {
        advanced.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Exactly one racing advance may move the ladder; the rest observe a stage
  // that no longer matches the evidence and hold.
  CHK(advanced.load() == 1);
  const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
  CHK(plan != nullptr && plan->stage_index == 1);
  CHK(plan != nullptr && plan->stage_history.size() == 2);
  CHK(plan != nullptr && plan->total_attempts == 2);
}

void test_concurrent_policy_and_plan_reads() {
  SECTION("concurrent policy and plan reads are stable");
  Fixture fx;
  CHK(fx.build());
  std::atomic<bool> failed{false};
  std::thread reader([&]() {
    for (int i = 0; i < 2000; ++i) {
      const RecoveryPolicy* policy = fx.engine.policy(fx.policy);
      if (policy == nullptr || policy->stages.size() != 6u) {
        failed.store(true);
      }
      const RecoveryPlan* plan = fx.engine.plan(fx.handle.id);
      if (plan == nullptr || !is_valid(plan->state)) {
        failed.store(true);
      }
    }
  });
  std::uint64_t tick = 1000;
  std::uint64_t authority = 100;
  for (std::size_t i = 0; i < 5; ++i) {
    tick += 20;
    const RestorationVector applied = target_for(i);
    (void)step(fx, tick, rate_for(i), true, &applied);
    RecoveryAuthority grant{};
    grant.id = AuthorityId(authority);
    grant.generation = AuthorityGeneration(authority);
    grant.epoch = fx.epoch;
    grant.resource = kR1;
    grant.unit = RestorationUnit::BYTES_PER_SECOND;
    grant.ceiling = kBaselineR1;
    grant.expires_at_tick = 100000000;
    (void)fx.engine.grant_authority(grant);
    ++authority;
  }
  reader.join();
  CHK(!failed.load());
}

}  // namespace

int main() {
  crtest::init();
  test_concurrent_evaluation_is_deterministic();
  test_concurrent_completion_is_single_commit();
  test_mixed_concurrent_load();
  test_concurrent_advance_has_one_winner_per_stage();
  test_concurrent_policy_and_plan_reads();
  return summary("cr_concurrency");
}
