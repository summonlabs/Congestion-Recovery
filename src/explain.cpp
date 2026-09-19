// Congestion Recovery - deterministic explanation rendering.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/explain.hpp"

#include <string>

namespace congestion_recovery {
namespace {

constexpr std::size_t kMaxRenderedLines = 512;

void add(std::vector<std::string>& lines, std::string text) {
  if (lines.size() >= kMaxRenderedLines) {
    return;
  }
  lines.push_back(std::move(text));
}

std::string number(std::uint64_t value) { return std::to_string(value); }

}  // namespace

std::string RecoveryExplanation::render() const {
  std::vector<std::string> lines;
  lines.reserve(64);

  {
    std::string s = "plan=";
    s += number(plan.value());
    s += " generation=";
    s += number(plan_generation.value());
    s += " epoch=";
    s += number(epoch.value());
    s += " policy=";
    s += number(policy_id.value());
    s += " policy_generation=";
    s += number(policy_generation.value());
    add(lines, std::move(s));
  }
  {
    std::string s = "state=";
    s += to_string(state);
    s += " stage=";
    s += to_string(stage_kind);
    s += " stage_index=";
    s += number(stage_index);
    s += "/";
    s += number(stage_count);
    s += " stage_generation=";
    s += number(stage_generation.value());
    s += " attempt=";
    s += number(attempt.value());
    add(lines, std::move(s));
  }
  {
    std::string s = "restoration_amount=";
    std::uint64_t requested_total = 0;
    std::uint64_t effective_total = 0;
    std::uint64_t observed_total = 0;
    for (const RestorationView& view : restoration) {
      requested_total += view.requested;
      effective_total += view.effective;
      if (view.observed_known) {
        observed_total += view.observed;
      }
    }
    s += number(requested_total);
    s += " effective=";
    s += number(effective_total);
    s += " observed=";
    s += number(observed_total);
    add(lines, std::move(s));
  }
  {
    std::string s = "reasons stale=";
    s += to_string(stale_reason);
    s += " pause=";
    s += to_string(pause_reason);
    s += " detail=";
    s += reason_text;
    add(lines, std::move(s));
  }
  {
    std::string s = "budget dwell_remaining=";
    s += number(dwell_remaining_ticks);
    s += " cooldown_remaining=";
    s += number(cooldown_remaining_ticks);
    s += " attempts_in_stage=";
    s += number(attempts_in_stage);
    s += " total_attempts=";
    s += number(total_attempts);
    s += " rollbacks=";
    s += number(rollback_count);
    s += " completions=";
    s += number(completion_count);
    s += " revalidation_required=";
    s += revalidation_required ? "yes" : "no";
    add(lines, std::move(s));
  }
  for (const EvidenceEvaluation& ev : required_evidence) {
    std::string s = "required_evidence ";
    s += to_string(ev.kind);
    s += " ";
    s += ev.detail;
    add(lines, std::move(s));
  }
  for (const EvidenceEvaluation& ev : observed_evidence) {
    std::string s = "observed_evidence ";
    s += to_string(ev.kind);
    s += " satisfied=";
    s += ev.satisfied ? "yes" : "no";
    s += " ";
    s += ev.detail;
    add(lines, std::move(s));
  }
  for (const RestorationView& view : restoration) {
    std::string s = "restoration resource=";
    s += number(view.resource.value());
    s += " unit=";
    s += to_string(view.unit);
    s += " baseline=";
    s += view.baseline_known ? number(view.baseline) : std::string("UNKNOWN");
    s += " constrained=";
    s += number(view.constrained);
    s += " requested=";
    s += number(view.requested);
    s += " authorized=";
    s += number(view.authorized);
    s += " effective=";
    s += number(view.effective);
    s += " observed=";
    s += view.observed_known ? number(view.observed) : std::string("UNKNOWN");
    s += " rollback_target=";
    s += number(view.rollback_target);
    s += " authority_clamped=";
    s += view.authority_clamped ? "yes" : "no";
    add(lines, std::move(s));
  }
  for (const ObligationEvaluation& ev : obligations) {
    std::string s = "protected_obligation id=";
    s += number(ev.id.value());
    s += " kind=";
    s += to_string(ev.kind);
    s += " state=";
    s += to_string(ev.state);
    s += " threshold=";
    s += number(ev.threshold);
    s += " candidate=";
    s += number(ev.candidate);
    s += " blocking=";
    s += ev.blocking ? "yes" : "no";
    add(lines, std::move(s));
  }
  for (const AuthorityView& view : authority) {
    std::string s = "authority resource=";
    s += number(view.resource.value());
    s += " present=";
    s += view.present ? "yes" : "no";
    s += " generation=";
    s += number(view.generation.value());
    s += " floor=";
    s += number(view.floor);
    s += " ceiling=";
    s += number(view.ceiling);
    s += " expired=";
    s += view.expired ? "yes" : "no";
    s += " epoch_match=";
    s += view.epoch_match ? "yes" : "no";
    add(lines, std::move(s));
  }
  for (const std::string& note : notes) {
    std::string s = "note ";
    s += note;
    add(lines, std::move(s));
  }

  std::string out;
  for (const std::string& line : lines) {
    out += line;
    out += "\n";
  }
  return out;
}

}  // namespace congestion_recovery
