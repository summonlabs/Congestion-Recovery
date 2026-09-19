# Contributing to Congestion Recovery

We welcome contributions from individuals and organizations.

## Contribution terms

Contributions are submitted under the terms of the Apache License 2.0. No
Contributor License Agreement (CLA) is required. By submitting a contribution
you agree that it may be distributed under the Apache License 2.0.

## Scope and boundaries

Keep changes inside the repository's architectural and system boundary.
Congestion Recovery owns post-intervention restoration sequencing and recovery
authority. It does not own congestion detection, generic admission, bandwidth
arbitration, rate enforcement, pacing, queue/buffer implementation, path
computation, traffic-engineering allocation, backpressure propagation, or
physical forwarding. Do not expand scope into adjacent systems or sibling
repositories; express the need as an explicit adjacent-runtime request instead.

## Quality expectations

- C++20, strict warnings, no warnings tolerated in first-party code.
- Every authoritative decision must be deterministic, explainable and bound to
  the exact generations and evidence that justified it.
- Missing, stale or contradictory evidence must never become positive authority.
- Add or extend a test for every behavioural change, including a failure or
  adversarial case where one applies.
- No test timeouts. A hanging test is a defect to diagnose, not to hide.
- Run the full build and test suite, including the multiprocess suites, before
  opening a pull request, and leave the working tree clean.

## Attribution

Do not add AI attribution, Co-authored-by trailers or generated-by notices to
commits, source files or documentation.

## Reporting

Report defects with the exact command, the observed decision and reason code,
and the evidence generations involved. Reproducibility matters more than prose.
