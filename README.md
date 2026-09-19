# Congestion Recovery

**Congestion Recovery is an open-source, vendor-neutral C++20 runtime for post-intervention restoration governance: staged restoration, hysteresis, revalidation, compensating rollback and recovery authority.**

**It answers one systems question:**

**Given a prior congestion intervention, current authoritative evidence, recovery policy, protected obligations and exact generations, is it safe to restore traffic now, by how much and in what stages, what proves each stage succeeded, and when must recovery pause, roll back, or be rejected as stale?**

Congestion clearing is not recovery completion. One good sample is not proof of stability. Recovery intent is not applied effect. Rollback is not simply reversing a vector. A stale baseline cannot be restored blindly. Congestion Recovery exists to make those distinctions explicit, generation-bound and testable.

## Defining thesis

**Recovery is a governed sequence of evidence-bound transitions, not a ramp back to a remembered number.**

Restoration that is not bound to the exact evidence, policy, resource, plan and coordinator generations that justified it is not authority. A higher service rate observed once proves nothing about stability. A stage that was *requested* is not a stage that was *applied*, and a stage that was applied is not a stage that was observed. When the current state is unknown, the correct compensating action is to say so, not to invert an earlier request.

## Systems boundary

Congestion Recovery is intentionally narrow. It does not absorb the responsibilities of adjacent systems:

- **Congestion Fabric** owns congestion detection, episode identity and congestion evidence.
- **Admission Fabric / Network Admission Fabric** own generic admission.
- **Bandwidth Broker / Bandwidth Governor** own bandwidth arbitration.
- **Rate Governor / Pacing Fabric** own rate enforcement and pacing.
- **Queue Fabric / Buffer Fabric / Unified Buffer** own queue and buffer implementation.
- **Path Planner / Path Authority / Route Fabric** own path computation and path authority.
- **Traffic Engineering Fabric** owns traffic-engineering allocation.
- **Backpressure Fabric** owns backpressure propagation.
- **Recovery Planner** owns general recovery planning across unrelated failure domains.

**Congestion Recovery** owns post-intervention restoration sequencing and recovery authority: recovery-plan identity, congestion/intervention references, restoration stages and attempts, evidence snapshots bound to exact generations, protected obligations, hold/dwell periods, hysteresis bands, revalidation boundaries, compensating rollback plans, completion lineage, recovery epochs and explanation.

It never detects congestion, admits traffic, arbitrates bandwidth, enforces rates, paces, implements queues, computes paths, allocates traffic engineering, propagates backpressure or changes physical forwarding. Where an adjacent action is required it emits a bounded, generation-bound `AdjacentRequest` naming the owning runtime, and records intent only.

## What is implemented

- **Strongly typed identities, generations and epochs** for recovery plans, policies, congestion references, interventions, resources, evidence, evidence snapshots, stages, attempts, rollbacks, completions, obligations, authority, workers, worker boots, publisher incarnations and coordinator epochs. Identity value 0 is always INVALID. Generations never wrap: a bump past the maximum fails instead of regressing, and a stale generation cannot mutate current authoritative state.
- **An explicit six-rung ladder** `OBSERVE → PROBE → PARTIAL_RESTORE → HOLD → EXPAND → COMPLETE` with `PAUSED`, `ROLLING_BACK`, `ROLLED_BACK`, `STALE`, `REVALIDATION_REQUIRED`, `FAILED` and `ABORTED` paths. Occupying the `COMPLETE` rung is reported as `VERIFYING`, never as completed: completion is committed only when the applied restoration has been observed.
- **Restoration vectors with checked arithmetic.** Levels are absolute per-resource values in a declared unit; every externally influenced size, capacity, counter and time unit is checked or saturating.
- **Evidence snapshots bound to exact generations.** Each entry carries its plan, stage, policy, resource and coordinator epoch binding, publisher, worker, boot, incarnation, provenance, confidence, sample count, duration, stability and affirmation. Requirements specify minimum provenance, confidence, samples, duration, stability, affirmation and value bounds.
- **Deterministic hysteresis.** A primary requirement per rung drives an advance band, a hold band and a recurrence band. Evidence inside the hold band can never oscillate: the plan holds. A bounded transition-rate window stops a plan from churning even when every individual transition is legal.
- **Protected obligations** (`MIN_SERVICE_RATE`, `RESERVED_CAPACITY`, `PROTECTED_CLASS_FLOOR`, `PRIORITY_FLOOR`, `MAX_QUEUE_DEPTH`, `MAX_LATENCY`) with hard and soft severity. A hard obligation is enforced at plan creation against the rollback target and the constrained state, and at every transition against the candidate restoration. Obligations reject rather than silently clamp.
- **Recovery authority** as live, epoch-bound, expiring grants per resource with a floor and a ceiling. Restoration never exceeds current legal authority: the effective level is clamped to the grant, and a missing, expired or foreign-epoch grant refuses the transition. Authority is never restored from durable storage.
- **Revalidation boundaries.** Exceeding the boundary interval, changing a policy generation, changing a resource generation, advancing the coordinator epoch, restarting, or losing a worker mid-attempt all push a plan into `REVALIDATION_REQUIRED`; a plan must be revalidated against current facts before it can advance again.
- **Compensating rollback recomputed from observed state.** The compensating plan is derived from the observed current levels toward the rollback target, never from the previously requested vector. Resources whose observed level is unknown are listed as unobserved and are never guessed; a rollback whose observation is missing, synthetic or structurally invalid is refused.
- **Completion lineage with idempotent replay and permanent failure.** A completion is keyed by the exact plan generation, stage generation, evidence generation and attempt. Replaying that key returns the recorded outcome and mutates nothing. A failed completion can never later be presented as success. A completion arriving under an older epoch or plan generation is stale work and is rejected.
- **Deterministic explanations** exposing current stage, required and observed evidence, requested/authorized/effective/observed restoration per resource, protected constraint state, rollback target, stale and pause reasons, dwell and cooldown remaining, attempts, rollbacks, completions and the authority vector.
- **Versioned, integrity-checked durability.** A versioned binary snapshot with CRC-32 and strict structural validation, plus an append-only checksummed journal with monotonic sequence numbers and epoch stamping. Snapshots are replaced atomically so a crash leaves either the previous complete snapshot or the new one. Restart restores durable configuration, plan lineage, committed completions and rollback history, and explicitly does **not** restore liveness, live authority, telemetry freshness or unfinished attempts.
- **A real OS-process reference deployment** over framed, checksummed TCP: one `crcoordinator` process holding the epoch, plan, policy and journal plus `crworker` processes that publish evidence and apply bounded stage grants. Real worker kill mid-stage, attempt-ambiguity marking, boot fencing, stale-incarnation and stale-epoch replay rejection, coordinator restart with epoch advancement, durable plan recovery and safe revalidation are all proven with real processes.

## Separations this runtime enforces

| Not this | But this |
| --- | --- |
| Congestion cleared | Recovery complete: the `COMPLETE` rung reports `VERIFYING` until applied restoration is observed |
| One good sample | Proof of stability: a stage needs a stability window, a duration, a sample count and a provenance floor |
| Recovery intent | Applied effect: the requested level, the authorized level, the effective level and the observed level are four distinct values |
| Reversing a vector | Compensating from observed state, with unobserved resources listed rather than assumed |
| Restoring a remembered baseline | Revalidating current facts before any restoration resumes |

## Invariants

- Restoration never exceeds current legal authority.
- Protected obligations remain protected.
- Stale evidence cannot advance a stage.
- Failed completion cannot be reused.
- Recovery cannot oscillate without bounded hysteresis.
- Rollback produces an explicit compensating plan.
- Duplicate completion is idempotent.

## Hardening covered

Congestion recurrence during recovery, capacity drop, path change, policy change, delayed completion, worker death, coordinator restart, stale completion replay, partial application and corrupt persistence are each covered by a dedicated test or scenario, in-process and across real OS processes.

## Durability model

The durable unit is a validated snapshot plus an append-only journal:

```
validate -> bind authority -> plan -> reserve -> journal intent -> apply -> verify -> snapshot -> retire
```

A mutation is never acknowledged before the required state is durable. On restart the coordinator advances the epoch, imports the snapshot, restores durable configuration and committed history, drops every live authority grant, and requires revalidation before any restored plan may advance. An intent record without its commit marker is reported as an unfinished attempt whose outcome is ambiguous.

## Repository layout

- `include/congestion_recovery/` - public headers.
- `src/` - plane engine, restoration and obligation evaluation, policy validation, explanation, snapshot/journal serialization, framed transport, coordinator and durable store.
- `tools/` - `crcoordinator`, `crworker` and `crcli` (offline inspection).
- `tests/` - smoke, stages, rollback, obligations, persistence, adversarial, property, concurrency, multiprocess and coordinator-restart suites, plus an independent downstream consumer.
- `examples/` - runnable staged restoration and recurrence/rollback examples.
- `benchmarks/` - synthetic recovery-plan evaluation throughput.
- `cmake/` - package config. The installed target is `CongestionRecovery::CongestionRecovery`.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

MSVC builds use `/W4 /WX`; GCC and Clang builds use `-Wall -Wextra -Wpedantic -Werror`. `-DCR_ENABLE_ASAN=ON` enables AddressSanitizer. The multiprocess suites require PowerShell and the reference tools and are registered automatically when both are available.

## Proof surface

| Claim | Status |
| --- | --- |
| Staged restoration ladder, hysteresis, dwell, cooldown, transition-rate bound | REAL (in-process, deterministic) |
| Protected obligations and recovery authority enforcement | REAL (in-process, deterministic) |
| Compensating rollback recomputed from observed state | REAL (in-process, deterministic) |
| Stale evidence, stale plan, stale epoch, stale completion rejection | REAL (in-process and across processes) |
| Versioned snapshot, CRC integrity, journal replay, restart conservatism | REAL (real files on disk) |
| Worker kill mid-stage, boot fencing, revalidation after replacement | REAL (separate OS processes, framed loopback TCP) |
| Coordinator restart, epoch advancement, durable plan recovery | REAL (separate OS processes, real durable state) |
| Recovery-plan evaluation and transition throughput | SYNTHETIC (in-memory plan transitions only) |
| Physical recovery time on real links, switches, NICs, RDMA or optical fabric | UNSUPPORTED - not measured and not claimed |

## Sanitizers and static analysis

- **Static analysis**: MSVC `/analyze` runs clean over every first-party translation unit. The only findings reported are inside the Windows SDK header `ws2tcpip.h`, not in this repository.
- **Debug heap validation**: Debug builds enable the MSVC debug heap (`_CRTDBG_ALLOC_MEM_DF`, `_CRTDBG_CHECK_DEFAULT_DF`, `_CRTDBG_LEAK_CHECK_DF`), and every suite asserts at exit that the live block and byte counts returned to their starting point. Debug iterator checking is active for every container in the test and library code.
- **AddressSanitizer**: the machine used for this release does not have the MSVC AddressSanitizer component installed (the `clang_rt.asan_*` runtime libraries are absent), so `-DCR_ENABLE_ASAN=ON` cannot link here and **no AddressSanitizer result is claimed**. The option is wired for toolchains that do have the component.
- **Concurrency**: the engine holds exactly one mutex, never invokes a callback or performs I/O while holding it, owns no threads and joins nothing, so read-to-write lock re-entry, mutex re-entry through callbacks, event emission under a lock and reversed lock ordering are structurally absent rather than merely untested. The concurrency suite drives every public entry point from eight threads and asserts single-commit completion and one-winner-per-stage advancement.

## Benchmark honesty

The benchmark measures completed in-memory recovery-plan evaluations and transitions over a synthetic population. It is not a physical recovery-time claim, and no packet, link, switch, NIC, DPU or fabric is involved. Results depend on the host and are reported by the benchmark itself when it is run.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
