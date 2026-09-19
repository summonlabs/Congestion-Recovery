// Congestion Recovery - bounded adjacent-runtime request log.
// Copyright 2026 Summon Software Labs.
#include "congestion_recovery/adjacent.hpp"

namespace congestion_recovery {

bool AdjacentRequestLog::push(const AdjacentRequest& request) {
  if (!is_valid(request.kind) || !request.plan.is_valid()) {
    return false;
  }
  if (entries_.size() >= static_cast<std::size_t>(capacity_)) {
    // Oldest-first eviction keeps memory bounded while counting the loss.
    entries_.erase(entries_.begin());
    ++dropped_;
  }
  entries_.push_back(request);
  return true;
}

std::vector<std::string> AdjacentRequestLog::render(std::uint32_t limit) const {
  std::vector<std::string> out;
  if (limit == 0u) {
    return out;
  }
  const std::size_t max_items = static_cast<std::size_t>(limit);
  const std::size_t start = entries_.size() > max_items ? entries_.size() - max_items : 0u;
  out.reserve(entries_.size() - start);
  for (std::size_t i = start; i < entries_.size(); ++i) {
    const AdjacentRequest& r = entries_[i];
    std::string s;
    s.reserve(96);
    s += to_string(r.kind);
    s += " owner=";
    s += r.owner_runtime;
    s += " plan=";
    s += std::to_string(r.plan.value());
    s += " resource=";
    s += std::to_string(r.resource.value());
    s += " amount=";
    s += std::to_string(r.amount);
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace congestion_recovery
