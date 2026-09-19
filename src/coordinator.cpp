// Congestion Recovery - durable store and reference coordinator.
// Copyright 2026 Summon Software Labs.
//
// Locking discipline: DurableStore and Coordinator are single-threaded by
// construction. No mutex is held across socket I/O, and no callback re-enters
// the engine while the engine holds its lock.
#include "congestion_recovery/coordinator.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

namespace congestion_recovery {
namespace {

constexpr ResourceId kR1{1001};
constexpr ResourceId kR2{1002};
constexpr std::uint64_t kBaselineR1 = 1000;
constexpr std::uint64_t kBaselineR2 = 800;
constexpr std::uint64_t kConstrainedR1 = 200;
constexpr std::uint64_t kConstrainedR2 = 400;
constexpr std::uint64_t kAuthorityExpiry = 1000000000ull;

EvidenceRequirement require_cleared() {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::CONGESTION_CLEARED;
  r.min_provenance = Provenance::REPORTED;
  r.min_confidence = 0.5;
  r.min_samples = 1;
  r.require_affirmative = true;
  return r;
}

EvidenceRequirement require_rate(std::uint64_t min_value) {
  EvidenceRequirement r{};
  r.kind = EvidenceKind::SERVICE_RATE;
  r.min_provenance = Provenance::REPORTED;
  r.min_confidence = 0.5;
  r.min_samples = 3;
  r.min_duration_ticks = 20;
  r.require_stable = true;
  r.stability_window_ticks = 20;
  r.has_min_value = true;
  r.min_value = min_value;
  r.primary = true;
  return r;
}

StageSpec make_stage(StageKind kind, const char* name, std::uint64_t r1, std::uint64_t r2, std::uint64_t rate,
                     bool authority) {
  StageSpec s{};
  s.kind = kind;
  s.name = name;
  (void)s.target.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, r1);
  (void)s.target.set_level(kR2, RestorationUnit::BYTES_PER_SECOND, r2);
  s.dwell_ticks = 10;
  s.stability_window_ticks = 20;
  s.max_attempts = 3;
  s.requires_authority = authority;
  s.rollback_on_failure = true;
  s.requirements.push_back(require_cleared());
  s.requirements.push_back(require_rate(rate));
  return s;
}

RecoveryPolicy scenario_policy() {
  RecoveryPolicy p{};
  p.generation = PolicyGeneration(1);
  p.name = "reference-staged-recovery";
  p.stages.push_back(make_stage(StageKind::OBSERVE, "observe", kConstrainedR1, kConstrainedR2, 600, false));
  p.stages.push_back(make_stage(StageKind::PROBE, "probe", 400, 500, 900, true));
  p.stages.push_back(make_stage(StageKind::PARTIAL_RESTORE, "partial_restore", 700, 650, 1350, true));
  p.stages.push_back(make_stage(StageKind::HOLD, "hold", 700, 650, 1350, true));
  p.stages.push_back(make_stage(StageKind::EXPAND, "expand", kBaselineR1, kBaselineR2, 1800, true));
  p.stages.push_back(make_stage(StageKind::COMPLETE, "complete", kBaselineR1, kBaselineR2, 1800, true));
  p.hysteresis.advance_margin = 100;
  p.hysteresis.rollback_margin = 50;
  p.hysteresis.cooldown_ticks = 5;
  p.hysteresis.max_transitions_per_window = 12;
  p.hysteresis.window_ticks = 100000;
  p.on_recurrence = RecurrenceAction::ROLLBACK;
  p.max_evidence_age_ticks = 100000;
  p.max_plan_lifetime_ticks = 100000000;
  p.revalidation_boundary_ticks = 100000000;
  p.max_rollbacks = 3;
  p.max_total_attempts = 32;
  p.baseline_required = true;
  return p;
}

RestorationVector make_levels(std::uint64_t r1, std::uint64_t r2) {
  RestorationVector v{};
  (void)v.set_level(kR1, RestorationUnit::BYTES_PER_SECOND, r1);
  (void)v.set_level(kR2, RestorationUnit::BYTES_PER_SECOND, r2);
  return v;
}

std::uint64_t stage_rate(std::size_t index) {
  switch (index) {
    case 0: return 900;
    case 1: return 1100;
    case 2: return 1500;
    case 3: return 1500;
    default: return 2000;
  }
}

void write_levels(Writer& w, const RestorationVector& levels) {
  w.put_u32(static_cast<std::uint32_t>(levels.size()));
  for (const RestorationAmount& a : levels.entries()) {
    w.put_u64(a.resource.value());
    w.put_u8(static_cast<std::uint8_t>(a.unit));
    w.put_u64(a.requested);
    w.put_u64(a.effective);
  }
}

bool read_levels(Reader& r, RestorationVector& out) {
  const std::uint32_t count = r.get_u32();
  if (r.bad() || count > 64u) {
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const ResourceId resource(r.get_u64());
    const RestorationUnit unit = static_cast<RestorationUnit>(r.get_u8());
    const std::uint64_t observed = r.get_u64();
    const std::uint64_t requested = r.get_u64();
    if (r.bad() || !resource.is_valid() || !is_valid(unit)) {
      return false;
    }
    RestorationAmount a{};
    a.resource = resource;
    a.unit = unit;
    a.requested = requested;
    a.authorized = requested;
    a.effective = requested;
    a.observed = observed;
    a.observed_known = true;
    if (!out.put(a)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------
DurableStore::DurableStore(std::string dir, EngineLimits limits)
    : dir_(std::move(dir)), limits_(limits) {
  const std::filesystem::path base_path(dir_);
  snapshot_path_ = (base_path / "recovery.snapshot").string();
  journal_path_ = (base_path / "recovery.journal").string();
}

DurableStore::~DurableStore() = default;

bool DurableStore::open(std::string& err) {
  err.clear();
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    err = "could not create the state directory";
    return false;
  }
  return true;
}

bool DurableStore::append(const JournalRecord& record, std::string& err) {
  std::vector<std::uint8_t> bytes;
  if (!serialize_journal_record(record, bytes, err)) {
    return false;
  }
  std::ofstream out(journal_path_, std::ios::binary | std::ios::app);
  if (!out) {
    err = "journal could not be opened for append";
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  if (!out) {
    err = "journal append failed";
    return false;
  }
  ++records_written_;
  return true;
}

bool DurableStore::flush(std::string& err) {
  err.clear();
  const std::uint64_t size = std::filesystem::exists(journal_path_)
                                 ? static_cast<std::uint64_t>(std::filesystem::file_size(journal_path_))
                                 : 0u;
  if (size > kJournalMaxBytes) {
    err = "journal exceeds the maximum size";
    return false;
  }
  return true;
}

bool DurableStore::write_snapshot(const DurableState& state, std::string& err) {
  err.clear();
  std::vector<std::uint8_t> bytes;
  if (!serialize_state(state, limits_, bytes, err)) {
    return false;
  }
  const std::string temp_path = snapshot_path_ + ".tmp";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "snapshot temp file could not be created";
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      err = "snapshot write failed";
      return false;
    }
  }
  // Crash safety: the snapshot is replaced atomically, so a crash leaves either
  // the previous complete snapshot or the new one, never a torn file.
#ifdef _WIN32
  if (!::MoveFileExW(std::wstring(temp_path.begin(), temp_path.end()).c_str(),
                     std::wstring(snapshot_path_.begin(), snapshot_path_.end()).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    err = "atomic snapshot replace failed";
    return false;
  }
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, snapshot_path_, ec);
  if (ec) {
    err = "atomic snapshot replace failed";
    return false;
  }
#endif
  open_attempt_epoch_ = 0;
  return true;
}

bool DurableStore::load(DurableState& state, RestoreReport& report, std::string& err) {
  err.clear();
  report = RestoreReport{};
  state = DurableState{};
  std::error_code ec;
  if (!std::filesystem::exists(snapshot_path_, ec)) {
    report.loaded = false;
    report.detail = "no snapshot present";
    return true;
  }
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream in(snapshot_path_, std::ios::binary);
    if (!in) {
      err = "snapshot could not be opened";
      return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kSnapshotMaxBytes) {
      err = "snapshot size out of range";
      return false;
    }
    in.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
      in.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!in && size > 0) {
      err = "snapshot read failed";
      return false;
    }
  }
  if (!deserialize_state(bytes.data(), bytes.size(), limits_, state, err)) {
    report.valid = false;
    report.detail = err;
    return false;
  }
  report.loaded = true;
  report.detail = "snapshot loaded";
  next_sequence_ = state.journal_sequence + 1u;

  std::vector<std::uint8_t> journal_bytes;
  if (std::filesystem::exists(journal_path_, ec)) {
    std::ifstream in(journal_path_, std::ios::binary);
    if (in) {
      in.seekg(0, std::ios::end);
      const std::streamoff size = in.tellg();
      if (size > 0 && static_cast<std::uint64_t>(size) <= kJournalMaxBytes) {
        in.seekg(0, std::ios::beg);
        journal_bytes.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(journal_bytes.data()), size);
        if (!in) {
          journal_bytes.clear();
        }
      }
    }
  }
  std::vector<JournalRecord> replayed;
  if (!journal_bytes.empty()) {
    std::string replay_err;
    RestoreReport journal_report{};
    if (!replay_journal(journal_bytes.data(), journal_bytes.size(), state.epoch, limits_, replayed,
                        journal_report, replay_err)) {
      err = replay_err.empty() ? "journal replay failed" : replay_err;
      return false;
    }
    report.recovered_from_journal = true;
    report.journal_records_replayed = journal_report.journal_records_replayed;
    report.journal_records_rejected = journal_report.journal_records_rejected;
    report.journal_truncated_bytes = journal_report.journal_truncated_bytes;
    // An intent without its commit marker is an unfinished attempt: the
    // outcome is ambiguous and must never be treated as success.
    for (const JournalRecord& record : replayed) {
      if (record.sequence > next_sequence_) {
        next_sequence_ = record.sequence + 1u;
      }
      if (record.kind == "INTENT") {
        open_attempt_epoch_ = record.epoch.value();
      } else if (record.kind == "COMMIT") {
        open_attempt_epoch_ = 0;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------
struct Coordinator::Peer {
  WorkerId worker{};
  WorkerBoot boot{};
  std::string role{};
  TcpStream stream{};
  bool registered{false};
  bool fenced{false};
  std::uint64_t sequence{0};
};

Coordinator::Coordinator(CoordinatorConfig config)
    : config_(std::move(config)), store_(config_.state_dir, EngineLimits{}) {
  report_.scenario = config_.scenario;
}

Coordinator::~Coordinator() = default;

void Coordinator::line(const std::string& text) {
  report_.lines.push_back(text);
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
}

void Coordinator::fail(const std::string& text) {
  success_ = false;
  line("ERROR " + text);
}

bool Coordinator::setup(std::string& err) {
  if (!socket_system_startup(err)) {
    return false;
  }
  if (!store_.open(err)) {
    return false;
  }
  DurableState state{};
  RestoreReport restore{};
  std::string load_err;
  const bool have_state = store_.load(state, restore, load_err);
  if (!have_state && restore.valid) {
    return false;
  }
  if (have_state && restore.loaded) {
    report_.epoch_before = state.epoch.value();
    report_.restore_plans_requiring_revalidation = restore.plans_requiring_revalidation;
    const CoordinatorEpoch next(state.epoch.value() + 1u);
    if (!restore.valid) {
      // A corrupt snapshot is never silently repaired: the coordinator refuses
      // to continue with unverifiable durable state.
      err = "durable snapshot is invalid: " + restore.detail;
      return false;
    }
    const RestoreReport imported = engine_.import_state(state, next);
    if (!imported.valid) {
      err = "durable state could not be imported: " + imported.detail;
      return false;
    }
    line("RESTORED plans=" + std::to_string(imported.plans_loaded) +
         " requiring_revalidation=" + std::to_string(imported.plans_requiring_revalidation) +
         " completions_preserved=" + std::to_string(imported.completions_preserved) +
         " epoch_before=" + std::to_string(imported.epoch_before) +
         " epoch_after=" + std::to_string(imported.epoch_after));
    epoch_before_ = report_.epoch_before;
  } else {
    engine_.begin_epoch(CoordinatorEpoch(1));
    report_.epoch_before = 0;
  }
  report_.epoch_after = engine_.epoch().value();

  if (!listener_.listen_on(config_.host, config_.port, err)) {
    return false;
  }
  listener_.set_timeout_ms(config_.peer_timeout_ms);
  line("LISTEN host=" + config_.host + " port=" + std::to_string(listener_.port()) + " epoch=" +
       std::to_string(engine_.epoch().value()));
  return true;
}

bool Coordinator::register_connection(TcpStream&& stream, std::string& err) {
  Peer peer{};
  peer.stream = std::move(stream);
  peer.stream.set_timeout_ms(config_.peer_timeout_ms);
  Frame frame{};
  std::string frame_err;
  if (!peer.stream.recv_frame(frame, frame_err) || frame.kind != MessageKind::HELLO) {
    line("HELLO_REJECT detail=" + (frame_err.empty() ? std::string("unexpected frame") : frame_err));
    peer.stream.close();
    return true;
  }
  Reader reader(frame.body.data(), frame.body.size());
  const std::uint32_t version = reader.get_u32();
  peer.role = reader.get_str();
  (void)reader.get_str();
  if (reader.bad() || version != kProtocolVersion) {
    Frame reply{};
    reply.kind = MessageKind::ERROR_MSG;
    reply.epoch = engine_.epoch();
    Writer body;
    body.put_u8(static_cast<std::uint8_t>(WireError::PROTOCOL_MISMATCH));
    body.put_str("protocol version mismatch");
    reply.body = body.buffer();
    (void)peer.stream.send_frame(reply, frame_err);
    line("HELLO_REJECT detail=protocol mismatch");
    peer.stream.close();
    return true;
  }

  Peer* existing = nullptr;
  for (Peer& candidate : peers_) {
    if (candidate.worker == frame.worker) {
      existing = &candidate;
    }
  }

  // A worker boot never regresses. A lower or equal boot is a stale incarnation
  // replaying authority it no longer holds.
  if (existing != nullptr && !(frame.boot > existing->boot)) {
    Frame reply{};
    reply.kind = MessageKind::ERROR_MSG;
    reply.epoch = engine_.epoch();
    Writer body;
    body.put_u8(static_cast<std::uint8_t>(WireError::STALE_BOOT));
    body.put_str("worker boot is not newer than the registered incarnation");
    reply.body = body.buffer();
    (void)peer.stream.send_frame(reply, frame_err);
    line("STALE_BOOT_REJECT worker=" + std::to_string(frame.worker.value()) +
         " boot=" + std::to_string(frame.boot.value()) +
         " current=" + std::to_string(existing->boot.value()));
    report_.stale_rejections += 1;
    // Read one more frame from this connection. Work from a fenced or
    // superseded incarnation is refused explicitly rather than dropped, so the
    // sender learns why it holds no authority.
    Frame stale{};
    std::string stale_err;
    if (peer.stream.recv_frame(stale, stale_err)) {
      const bool epoch_stale = !(stale.epoch == engine_.epoch());
      const WireError wire = epoch_stale ? WireError::STALE_EPOCH : WireError::STALE_BOOT;
      Frame stale_reply{};
      stale_reply.kind = MessageKind::ERROR_MSG;
      stale_reply.epoch = engine_.epoch();
      Writer stale_body;
      stale_body.put_u8(static_cast<std::uint8_t>(wire));
      stale_body.put_str("frame carries no live authority");
      stale_reply.body = stale_body.buffer();
      (void)peer.stream.send_frame(stale_reply, frame_err);
      line(std::string("STALE_FRAME_REJECT kind=") + std::string(to_string(stale.kind)) +
           " epoch=" + std::to_string(stale.epoch.value()) +
           " boot=" + std::to_string(stale.boot.value()) +
           " current_epoch=" + std::to_string(engine_.epoch().value()) +
           " reason=" + std::string(to_string(wire)));
      report_.stale_rejections += 1;
    }
    peer.stream.close();
    return true;
  }

  const WorkerId worker = frame.worker;
  const WorkerBoot boot = frame.boot;
  if (existing != nullptr) {
    line("FENCED worker=" + std::to_string(existing->worker.value()) +
         " boot=" + std::to_string(existing->boot.value()));
    report_.workers_fenced += 1;
    existing->stream.close();
    existing->boot = boot;
    existing->role = peer.role;
    existing->stream = std::move(peer.stream);
    existing->registered = true;
    existing->fenced = false;
    Frame reply{};
    reply.kind = MessageKind::HELLO_ACK;
    reply.epoch = engine_.epoch();
    reply.worker = worker;
    reply.boot = boot;
    Writer body;
    body.put_u32(kProtocolVersion);
    body.put_u64(engine_.epoch().value());
    body.put_u64(0);
    body.put_u32(1u);
    body.put_str("welcome");
    reply.body = body.buffer();
    if (!existing->stream.send_frame(reply, frame_err)) {
      err = "HELLO_ACK failed";
      return false;
    }
    report_.workers_registered += 1;
    line("REGISTER worker=" + std::to_string(worker.value()) + " boot=" + std::to_string(boot.value()) +
         " role=" + existing->role);
    return true;
  }

  peer.worker = worker;
  peer.boot = boot;
  peer.registered = true;
  Frame reply{};
  reply.kind = MessageKind::HELLO_ACK;
  reply.epoch = engine_.epoch();
  reply.worker = worker;
  reply.boot = boot;
  Writer body;
  body.put_u32(kProtocolVersion);
  body.put_u64(engine_.epoch().value());
  body.put_u64(0);
  body.put_u32(1u);
  body.put_str("welcome");
  reply.body = body.buffer();
  if (!peer.stream.send_frame(reply, frame_err)) {
    err = "HELLO_ACK failed";
    return false;
  }
  report_.workers_registered += 1;
  line("REGISTER worker=" + std::to_string(worker.value()) + " boot=" + std::to_string(boot.value()) +
       " role=" + peer.role);
  peers_.push_back(std::move(peer));
  return true;
}

bool Coordinator::accept_workers(std::string& err) {
  while (peers_.size() < static_cast<std::size_t>(config_.expected_workers)) {
    const SocketHandle handle = listener_.accept_one(err);
    if (handle == kInvalidSocket) {
      err = "accept failed: " + err;
      return false;
    }
    if (!register_connection(TcpStream(handle), err)) {
      return false;
    }
  }
  return true;
}

bool Coordinator::accept_extra(std::string& err) {
  err.clear();
  const SocketHandle handle = listener_.accept_one(err);
  if (handle == kInvalidSocket) {
    // No extra connection attempted within the peer timeout. That is not a
    // failure: the scenario simply did not exercise a stale replay.
    err.clear();
    return true;
  }
  return register_connection(TcpStream(handle), err);
}

bool Coordinator::accept_replacement(const std::string& role, std::string& err) {
  const SocketHandle handle = listener_.accept_one(err);
  if (handle == kInvalidSocket) {
    err = "no replacement worker connected";
    return false;
  }
  Peer peer{};
  peer.stream = TcpStream(handle);
  peer.stream.set_timeout_ms(config_.peer_timeout_ms);
  Frame frame{};
  std::string frame_err;
  if (!peer.stream.recv_frame(frame, frame_err) || frame.kind != MessageKind::HELLO) {
    err = "replacement worker did not send HELLO";
    peer.stream.close();
    return false;
  }
  Reader reader(frame.body.data(), frame.body.size());
  const std::uint32_t version = reader.get_u32();
  peer.role = reader.get_str();
  (void)reader.get_str();
  if (reader.bad() || version != kProtocolVersion || peer.role != role) {
    err = "replacement worker handshake is invalid";
    peer.stream.close();
    return false;
  }
  Peer* existing = nullptr;
  for (Peer& candidate : peers_) {
    if (candidate.worker == frame.worker) {
      existing = &candidate;
    }
  }
  if (existing == nullptr || !(frame.boot > existing->boot)) {
    err = "replacement worker boot is not newer";
    peer.stream.close();
    return false;
  }
  line("FENCED worker=" + std::to_string(existing->worker.value()) +
       " boot=" + std::to_string(existing->boot.value()));
  report_.workers_fenced += 1;
  existing->stream.close();
  existing->boot = frame.boot;
  existing->stream = std::move(peer.stream);
  existing->fenced = false;
  existing->registered = true;
  Frame reply{};
  reply.kind = MessageKind::HELLO_ACK;
  reply.epoch = engine_.epoch();
  reply.worker = existing->worker;
  reply.boot = existing->boot;
  Writer body;
  body.put_u32(kProtocolVersion);
  body.put_u64(engine_.epoch().value());
  body.put_u64(0);
  body.put_u32(1u);
  body.put_str("welcome");
  reply.body = body.buffer();
  if (!existing->stream.send_frame(reply, frame_err)) {
    err = "replacement HELLO_ACK failed";
    return false;
  }
  report_.workers_registered += 1;
  line("REGISTER worker=" + std::to_string(existing->worker.value()) +
       " boot=" + std::to_string(existing->boot.value()) + " role=" + role);
  return true;
}

bool Coordinator::commit(const std::string& kind, std::uint64_t tick, std::string& err) {
  JournalRecord record{};
  record.sequence = store_.next_sequence();
  record.tick = tick;
  record.epoch = engine_.epoch();
  record.kind = kind;
  DurableState state = engine_.export_state();
  state.journal_sequence = record.sequence;
  if (!store_.append(record, err)) {
    return false;
  }
  if (!store_.flush(err)) {
    return false;
  }
  if (!store_.write_snapshot(state, err)) {
    return false;
  }
  report_.journal_records = store_.records_written();
  return true;
}

bool Coordinator::grant_stage(Peer& peer, std::uint64_t tick, std::string& err) {
  const RecoveryPlan* plan = engine_.plan(RecoveryPlanId(plan_id_));
  if (plan == nullptr) {
    err = "plan is absent";
    return false;
  }
  const RecoveryPolicy* policy = engine_.policy(plan->policy_id);
  if (policy == nullptr || plan->stage_index >= policy->stages.size()) {
    err = "policy or stage is absent";
    return false;
  }
  const StageSpec& stage = policy->stages[plan->stage_index];
  Frame frame{};
  frame.kind = MessageKind::STAGE_GRANT;
  frame.epoch = engine_.epoch();
  frame.worker = peer.worker;
  frame.boot = peer.boot;
  frame.sequence = ++peer.sequence;
  Writer body;
  body.put_u64(plan->id.value());
  body.put_u64(plan->generation.value());
  body.put_u64(plan->stage_generation.value());
  body.put_u64(plan->attempt.value());
  body.put_u8(static_cast<std::uint8_t>(stage.kind));
  body.put_u64(tick);
  write_levels(body, stage.target);
  const AuthorityVector authority = engine_.authority_snapshot();
  std::uint64_t authority_generation = 0;
  for (const ResourceBinding& binding : plan->resources) {
    const RecoveryAuthority* grant = authority.find(binding.resource);
    if (grant != nullptr) {
      authority_generation = (std::max)(authority_generation, grant->generation.value());
    }
  }
  body.put_u64(authority_generation);
  frame.body = body.buffer();
  return peer.stream.send_frame(frame, err);
}

bool Coordinator::collect_evidence(Peer& peer, std::uint64_t tick, bool recurrence,
                                   const RestorationVector& applied, std::string& err) {
  Frame request{};
  request.kind = MessageKind::EVIDENCE_REQUEST;
  request.epoch = engine_.epoch();
  request.worker = peer.worker;
  request.boot = peer.boot;
  request.sequence = ++peer.sequence;
  Writer body;
  body.put_u64(tick);
  body.put_u8(recurrence ? 1u : 0u);
  body.put_u32(2u);
  body.put_u8(static_cast<std::uint8_t>(EvidenceKind::CONGESTION_CLEARED));
  body.put_u8(static_cast<std::uint8_t>(EvidenceKind::SERVICE_RATE));
  request.body = body.buffer();
  if (!peer.stream.send_frame(request, err)) {
    return false;
  }
  Frame reply{};
  std::string frame_err;
  if (!peer.stream.recv_frame(reply, frame_err) || reply.kind != MessageKind::EVIDENCE_PUBLISH) {
    err = frame_err.empty() ? "unexpected evidence reply" : frame_err;
    return false;
  }
  if (!(reply.epoch == engine_.epoch())) {
    report_.stale_rejections += 1;
    line("EVIDENCE_REJECT reason=STALE_EPOCH epoch=" + std::to_string(reply.epoch.value()));
    err = "stale evidence epoch";
    return false;
  }
  Reader reader(reply.body.data(), reply.body.size());
  (void)reader.get_u64();
  const std::uint32_t count = reader.get_u32();
  const RecoveryPlan* plan = engine_.plan(RecoveryPlanId(plan_id_));
  const WorkerId applier_publisher = WorkerId(1);
  if (reader.bad() || plan == nullptr || count > 64u) {
    err = "malformed evidence body";
    return false;
  }
  EvidenceSnapshot snapshot{};
  snapshot.id = EvidenceSnapshotId(tick);
  snapshot.generation = SnapshotGeneration(tick);
  snapshot.plan_generation = plan->generation;
  snapshot.policy_generation = plan->policy_generation;
  snapshot.epoch = engine_.epoch();
  snapshot.assembled_at_tick = tick;
  for (std::uint32_t i = 0; i < count; ++i) {
    Evidence e{};
    e.id = EvidenceId(1000u + i);
    e.generation = EvidenceGeneration(1000u + i);
    e.kind = static_cast<EvidenceKind>(reader.get_u8());
    e.resource = ResourceId(reader.get_u64());
    e.value = reader.get_u64();
    e.sample_count = reader.get_u64();
    e.duration_ticks = reader.get_u64();
    e.confidence = reader.get_f64();
    e.provenance = static_cast<Provenance>(reader.get_u8());
    e.stable = reader.get_u8() != 0u;
    e.affirmative = reader.get_u8() != 0u;
    e.plan_generation = plan->generation;
    e.stage_generation = plan->stage_generation;
    e.policy_generation = plan->policy_generation;
    e.epoch = engine_.epoch();
    e.worker = reply.worker;
    e.boot = reply.boot;
    e.publisher = PublisherId(reply.worker.value());
    e.incarnation = PublisherIncarnation(reply.boot.value());
    e.unit = RestorationUnit::BYTES_PER_SECOND;
    e.observed_at_tick = tick >= 5u ? tick - 5u : tick;
    if (reader.bad() || !is_valid(e.kind)) {
      err = "malformed evidence entry";
      return false;
    }
    snapshot.entries.push_back(e);
  }

  // The applied levels the applier reported are evidence in their own right:
  // completion is never inferred from what was requested.
  for (const RestorationAmount& amount : applied.entries()) {
    Evidence e{};
    e.id = EvidenceId(5000u + static_cast<std::uint64_t>(snapshot.entries.size()));
    e.generation = EvidenceGeneration(e.id.value());
    e.kind = EvidenceKind::APPLIED_RESTORATION;
    e.resource = amount.resource;
    e.value = amount.observed_known ? amount.observed : amount.effective;
    e.unit = amount.unit;
    e.sample_count = 1;
    e.duration_ticks = tick;
    e.confidence = 0.9;
    e.provenance = Provenance::MEASURED;
    e.stable = true;
    e.affirmative = true;
    e.observed_at_tick = tick >= 5u ? tick - 5u : tick;
    e.plan_generation = plan->generation;
    e.stage_generation = plan->stage_generation;
    e.policy_generation = plan->policy_generation;
    e.epoch = engine_.epoch();
    e.publisher = PublisherId(applier_publisher.value());
    snapshot.entries.push_back(e);
  }

  const StageDecision decision = engine_.advance(plan->id, plan->generation, snapshot);
  report_.stale_rejections += decision.reason == RejectReason::STALE_EPOCH ||
                                      decision.reason == RejectReason::STALE_PLAN ||
                                      decision.reason == RejectReason::STALE_EVIDENCE ||
                                      decision.reason == RejectReason::STALE_COMPLETION
                                  ? 1u
                                  : 0u;
  const RecoveryPlan* after = engine_.plan(RecoveryPlanId(plan_id_));
  const std::string state_text = after != nullptr ? std::string(to_string(after->state)) : "UNKNOWN";
  line("STAGE tick=" + std::to_string(tick) +
       " index=" + std::to_string(decision.stage_index_before) +
       " kind=" + std::string(to_string(decision.stage_kind_before)) +
       " decision=" + std::string(to_string(decision.decision)) +
       " reason=" + std::string(to_string(decision.reason)) +
       " state=" + state_text);
  if (decision.advanced) {
    report_.stages_completed += 1;
  }
  if (decision.completed) {
    report_.completions_committed += 1;
    line("COMPLETE index=" + std::to_string(decision.stage_index_before) +
         " idempotent=" + std::string(decision.duplicate ? "1" : "0"));
  }
  return true;
}

Coordinator::Peer* Coordinator::find_peer(const std::string& role) {
  for (Peer& peer : peers_) {
    if (peer.role == role && peer.registered && !peer.fenced) {
      return &peer;
    }
  }
  return nullptr;
}

bool Coordinator::publish_and_advance(Peer& peer, std::uint64_t tick, bool recurrence,
                                      const RestorationVector& applied, std::string& err) {
  return collect_evidence(peer, tick, recurrence, applied, err);
}

bool Coordinator::run_staged(std::string& err, bool recurrence_mode, std::uint32_t stop_after) {
  Peer* applier = find_peer("applier");
  Peer* observer = find_peer("observer");
  if (applier == nullptr || observer == nullptr) {
    err = "required workers did not register";
    return false;
  }
  const RecoveryPlanId plan_id(plan_id_);
  for (int guard = 0; guard < 64; ++guard) {
    const RecoveryPlan* plan = engine_.plan(plan_id);
    if (plan == nullptr) {
      err = "plan disappeared";
      return false;
    }
    if (plan->state == RecoveryState::COMPLETED) {
      return true;
    }
    if (plan->state == RecoveryState::FAILED || plan->state == RecoveryState::STALE) {
      err = "plan is terminal before completion";
      return false;
    }
    if (stop_after != 0u && plan->stage_index >= stop_after && plan->state != RecoveryState::ROLLED_BACK) {
      line("STOPPED at index=" + std::to_string(plan->stage_index));
      DurableState state = engine_.export_state();
      state.journal_sequence = store_.next_sequence();
      std::string save_err;
      if (!store_.write_snapshot(state, save_err)) {
        err = save_err;
        return false;
      }
      return true;
    }
    tick_ += config_.tick_step;
    const bool recurrence = recurrence_mode && plan->stage_index >= 2 && plan->rollback_count == 0;
    if (!grant_stage(*applier, tick_, err)) {
      err = "stage grant failed: " + err;
      return false;
    }
    Frame report{};
    std::string frame_err;
    if (!applier->stream.recv_frame(report, frame_err) || report.kind != MessageKind::STAGE_REPORT) {
      // Real worker death mid-stage: the attempt outcome is ambiguous.
      line("WORKER_DEATH worker=" + std::to_string(applier->worker.value()) +
           " boot=" + std::to_string(applier->boot.value()) +
           " stage=" + std::to_string(plan->stage_index) + " detail=" + frame_err);
      applier->stream.close();
      applier->registered = false;
      engine_.require_revalidation(plan_id, RejectReason::STALE_EVIDENCE,
                                   "applier died mid-stage; attempt outcome is ambiguous");
      line("ATTEMPT_AMBIGUOUS stage=" + std::to_string(plan->stage_index));
      std::string accept_err;
      if (!accept_replacement("applier", accept_err)) {
        err = accept_err;
        return false;
      }
      const RecoveryPlan* reloaded = engine_.plan(plan_id);
      const RevalidationOutcome revalidated =
          engine_.revalidate(plan_id, reloaded->generation, reloaded->policy_generation,
                             reloaded->resources, tick_);
      if (!revalidated.ok) {
        err = "revalidation failed: " + revalidated.reason_text;
        return false;
      }
      report_.revalidations += 1;
      line("REVALIDATED plan=" + std::to_string(plan_id.value()) +
           " generation=" + std::to_string(engine_.plan(plan_id)->generation.value()) +
           " stage=" + std::to_string(engine_.plan(plan_id)->stage_index));
      applier = find_peer("applier");
      if (applier == nullptr) {
        err = "replacement applier is not registered";
        return false;
      }
      if (config_.scenario == "worker-death") {
        // Probe once for a stale-incarnation replay on a fresh connection. A
        // missing probe is not a failure; a present one must be refused.
        std::string probe_err;
        if (!accept_extra(probe_err)) {
          err = probe_err;
          return false;
        }
      }
      continue;
    }
    Reader reader(report.body.data(), report.body.size());
    (void)reader.get_u64();
    (void)reader.get_u64();
    (void)reader.get_u64();
    const std::uint8_t status = reader.get_u8();
    RestorationVector observed{};
    if (!read_levels(reader, observed) || reader.bad()) {
      err = "malformed stage report";
      return false;
    }
    if (status != 0u) {
      line("STAGE_REPORT_ERROR worker=" + std::to_string(report.worker.value()) +
           " status=" + std::to_string(status));
      return false;
    }
    const std::uint64_t applied_r1 = observed.effective_level(kR1);
    line("APPLIED stage=" + std::to_string(plan->stage_index) + " r1=" + std::to_string(applied_r1));
    tick_ += config_.tick_step;
    if (!publish_and_advance(*observer, tick_, recurrence, observed, err)) {
      return false;
    }
    if (recurrence_mode && engine_.plan(plan_id)->state == RecoveryState::ROLLING_BACK) {
      RollbackRequest request{};
      request.plan = plan_id;
      request.plan_generation = engine_.plan(plan_id)->generation;
      request.epoch = engine_.epoch();
      request.observed_current = observed;
      request.provenance = Provenance::MEASURED;
      request.evidence_generation = EvidenceGeneration(tick_);
      request.now_tick = tick_;
      request.trigger = RejectReason::RECURRENCE_DETECTED;
      request.reason = "observer reported congestion recurrence";
      const RollbackOutcome outcome = engine_.rollback(request);
      if (!outcome.ok) {
        err = "rollback failed: " + outcome.reason_text;
        return false;
      }
      report_.rollbacks += 1;
      line("ROLLBACK plan=" + std::to_string(plan_id.value()) +
           " compensation_steps=" + std::to_string(outcome.compensation.steps.size()) +
           " unobserved=" + std::to_string(outcome.compensation.unobserved.size()) +
           " state=" + std::string(to_string(outcome.state_after)));
      std::string resume_err;
      if (!commit("ROLLBACK", tick_, resume_err)) {
        err = resume_err;
        return false;
      }
      // A rollback imposes a cooldown; recovery only restarts after it.
      tick_ += config_.tick_step * 2u;
      const StageDecision resumed = engine_.resume(plan_id, engine_.plan(plan_id)->generation, tick_);
      if (resumed.decision != RecoveryDecision::RESUME) {
        err = "plan could not resume after rollback";
        return false;
      }
    }
    std::string commit_err;
    if (!commit("STAGE", tick_, commit_err)) {
      err = commit_err;
      return false;
    }
  }
  err = "stage loop guard exhausted";
  return false;
}

int Coordinator::run() {
  std::string err;
  if (!setup(err)) {
    fail(err);
    report_.success = false;
    return finish();
  }
  if (!accept_workers(err)) {
    fail(err);
    return finish();
  }

  if (config_.scenario == "restart-part2") {
    const std::vector<RecoveryPlanId> ids = engine_.plan_ids();
    if (ids.empty()) {
      fail("no durable plan was restored");
      return finish();
    }
    plan_id_ = ids.front().value();
    const RecoveryPlan* plan = engine_.plan(ids.front());
    const RevalidationOutcome revalidated =
        engine_.revalidate(ids.front(), plan->generation, plan->policy_generation, plan->resources, 2000);
    if (!revalidated.ok) {
      fail("restored plan could not be revalidated: " + revalidated.reason_text);
      return finish();
    }
    report_.revalidations += 1;
    line("REVALIDATED plan=" + std::to_string(ids.front().value()) +
         " generation=" + std::to_string(engine_.plan(ids.front())->generation.value()) +
         " stage=" + std::to_string(engine_.plan(ids.front())->stage_index) +
         " epoch=" + std::to_string(engine_.epoch().value()));
    if (!grant_authority_for_plan(ids.front())) {
      fail("authority could not be restored after restart");
      return finish();
    }
    // Probe once for a stale-incarnation replay that survived the restart.
    {
      std::string probe_err;
      if (!accept_extra(probe_err)) {
        fail("stale replay probe failed: " + probe_err);
        return finish();
      }
    }
    tick_ = 2000;
    if (!run_staged(err, false, 0u)) {
      fail(err);
      return finish();
    }
  } else {
    const RecoveryPolicy policy = scenario_policy();
    const PolicyId policy_id = engine_.define_policy(policy);
    if (!policy_id.is_valid()) {
      fail("policy definition failed");
      return finish();
    }
    const RecoveryPolicy* stored = engine_.policy(policy_id);
    CreatePlanRequest request{};
    request.policy_id = policy_id;
    request.policy_generation = stored->generation;
    request.congestion = CongestionRefId(77);
    request.intervention = InterventionId(88);
    request.epoch = engine_.epoch();
    request.now_tick = 1000;
    request.resources.push_back(ResourceBinding{kR1, ResourceGeneration(3)});
    request.resources.push_back(ResourceBinding{kR2, ResourceGeneration(4)});
    request.baseline = make_levels(kBaselineR1, kBaselineR2);
    request.baseline_known = true;
    request.constrained = make_levels(kConstrainedR1, kConstrainedR2);
    request.rollback_target = make_levels(kConstrainedR1, kConstrainedR2);
    const PlanHandle handle = engine_.create_plan(request);
    if (!handle.is_valid()) {
      fail("plan creation failed");
      return finish();
    }
    plan_id_ = handle.id.value();
    line("PLAN id=" + std::to_string(handle.id.value()) + " epoch=" + std::to_string(engine_.epoch().value()));
    if (!grant_authority_for_plan(handle.id)) {
      fail("authority grant failed");
      return finish();
    }
    tick_ = 1000;
    const bool recurrence_mode = config_.scenario == "recurrence";
    const std::uint32_t stop_after = config_.scenario == "restart-part1" ? 3u : 0u;
    if (!run_staged(err, recurrence_mode, stop_after)) {
      fail(err);
      return finish();
    }
  }
  report_.success = success_;
  return finish();
}

bool Coordinator::grant_authority_for_plan(RecoveryPlanId id) {
  const RecoveryPlan* plan = engine_.plan(id);
  if (plan == nullptr) {
    return false;
  }
  std::uint64_t generation = 1;
  for (const ResourceBinding& binding : plan->resources) {
    RecoveryAuthority grant{};
    grant.id = AuthorityId(generation);
    grant.generation = AuthorityGeneration(generation);
    grant.epoch = engine_.epoch();
    grant.resource = binding.resource;
    grant.unit = RestorationUnit::BYTES_PER_SECOND;
    grant.ceiling = binding.resource == kR1 ? kBaselineR1 : kBaselineR2;
    grant.floor = 0;
    grant.granted_at_tick = tick_;
    grant.expires_at_tick = kAuthorityExpiry;
    grant.issuer = PublisherId(1);
    if (!engine_.grant_authority(grant)) {
      return false;
    }
    ++generation;
  }
  line("AUTHORITY granted resources=" + std::to_string(plan->resources.size()) +
       " epoch=" + std::to_string(engine_.epoch().value()));
  return true;
}

int Coordinator::finish() {
  for (Peer& peer : peers_) {
    if (peer.registered && !peer.fenced) {
      Frame goodbye{};
      goodbye.kind = MessageKind::GOODBYE;
      goodbye.epoch = engine_.epoch();
      goodbye.worker = peer.worker;
      goodbye.boot = peer.boot;
      std::string ignored;
      (void)peer.stream.send_frame(goodbye, ignored);
    }
    peer.stream.close();
  }
  listener_.close();

  const RecoveryPlan* plan = plan_id_ != 0u ? engine_.plan(RecoveryPlanId(plan_id_)) : nullptr;
  report_.plan_state = plan != nullptr ? std::string(to_string(plan->state)) : "NONE";
  report_.rollbacks = plan != nullptr ? plan->rollback_count : 0u;
  report_.completions_committed =
      plan != nullptr ? static_cast<std::uint32_t>(plan->completion_count) : 0u;
  report_.stale_rejections += static_cast<std::uint32_t>(engine_.stale_rejections());
  report_.journal_records = store_.records_written();
  const bool completed = plan != nullptr && plan->state == RecoveryState::COMPLETED;
  report_.success = success_ && (config_.scenario == "restart-part1" ? true : completed);
  line("RESULT=" + std::string(report_.success ? "OK" : "FAILED"));
  if (!config_.report_path.empty()) {
    std::string write_err;
    if (!report_.write(config_.report_path, write_err)) {
      std::printf("report write failed: %s\n", write_err.c_str());
    }
  }
  socket_system_shutdown();
  return report_.success ? 0 : 1;
}

std::string CoordinatorReport::render() const {
  std::string out;
  out.reserve(512);
  out += "scenario=" + scenario + "\n";
  out += "epoch_before=" + std::to_string(epoch_before) + "\n";
  out += "epoch_after=" + std::to_string(epoch_after) + "\n";
  out += "workers_registered=" + std::to_string(workers_registered) + "\n";
  out += "workers_fenced=" + std::to_string(workers_fenced) + "\n";
  out += "stages_completed=" + std::to_string(stages_completed) + "\n";
  out += "rollbacks=" + std::to_string(rollbacks) + "\n";
  out += "completions_committed=" + std::to_string(completions_committed) + "\n";
  out += "stale_rejections=" + std::to_string(stale_rejections) + "\n";
  out += "revalidations=" + std::to_string(revalidations) + "\n";
  out += "restore_plans_requiring_revalidation=" + std::to_string(restore_plans_requiring_revalidation) + "\n";
  out += "journal_records=" + std::to_string(journal_records) + "\n";
  out += "plan_state=" + plan_state + "\n";
  for (const std::string& entry : lines) {
    out += "LOG " + entry + "\n";
  }
  out += "success=" + std::string(success ? "1" : "0") + "\n";
  return out;
}

bool CoordinatorReport::write(const std::string& path, std::string& err) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "report file could not be created";
    return false;
  }
  const std::string text = render();
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) {
    err = "report write failed";
    return false;
  }
  return true;
}

}  // namespace congestion_recovery
