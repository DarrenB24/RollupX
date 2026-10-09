# RollupX implementation plan

## Execution rules and current environment

Work in phase order; compile and run relevant tests before promoting a milestone.
Never return placeholder success for absent consensus, execution, or settlement.
This plan is the status ledger; design documents describe future behavior.

Initial inspection (2026-10-09): empty directory, Apple Clang 21 and OpenSSL 3
installed. No CMake, Ninja, protoc, gRPC, RocksDB, GoogleTest, Docker, forge, or
anvil on PATH. GitHub DNS lookup failed during dependency download. No parent
AGENTS.md was found. Work is restricted to the workspace and temporary directories.

## Phase 0 — specification (prerequisite for implementation)

- [x] Inspect repository, compiler, dependencies, and available execution tools.
- [x] Write architecture, trust boundaries, ownership and synchronization design.
- [x] Specify initial canonical transfer model, bounds, registry, and commitments.
- [x] Document Raft durability/recovery invariants and snapshot requirements.
- [x] Specify a bounded authenticated fraud witness and its limitations.
- [x] Record technical risks and performance measurement methodology.

Acceptance: documents distinguish target design from implemented behavior and
identify major persistence, determinism, and fraud-verification risks.

## Phase 1 — project foundation (depends on Phase 0; source written, gate blocked)

The checked items below indicate implementation source exists. Runtime verification
is recorded separately; this phase is **not accepted** until the full build and
five-node tests pass.

- [x] C++20 CMake targets, strict warnings, sanitizers, dependency discovery.
- [x] Documented protobuf transaction/public API/internal protocol schemas.
- [x] Validated static topology, exclusive durable node identity, bounded RPCs.
- [x] Separate public/internal gRPC servers; actual configuration-based peer
      identity checks (not cryptographic authentication).
- [x] HTTP liveness/readiness/Prometheus endpoints and graceful shutdown.
- [x] CLI connectivity/status commands; missing ledger methods fail explicitly.
- [x] GoogleTest config/storage/network tests, including five-node mesh and restart.
- [x] Five independent Docker services/volumes and reproducible smoke script.
- [x] CI build/unit/integration/sanitizer jobs (configured, not run remotely).
- [ ] Verify full CMake build and five processes exchanging gRPC messages.

Acceptance: all five processes observe the other four through actual gRPC; wrong
cluster/membership is rejected; deadlines bound unavailable peers; restart retains
identity and rejects mismatched reuse; ports shut down cleanly. Full dependency
tests must pass before Phase 1 is claimed complete. A dependency-free validation
path may verify portable modules but is not a substitute for networking tests.

## Phase 2 — transaction engine (depends on verified Phase 1)

- [ ] Golden canonical payload/ID/signature vectors and Ethereum Keccak verification.
- [ ] OpenSSL secp256k1 signing, public-key recovery, strict low-s validation.
- [ ] Immutable funded genesis registry, checked uint64 arithmetic and nonce rules.
- [ ] RocksDB atomic batch execution, receipts, durable IDs, applied marker.
- [ ] Retry semantics, concurrent duplicate admission, deterministic rejection.
- [ ] CLI account generation/signing, query balances/nonces/receipts.
- [ ] Process-kill hooks before/after storage effects and replay tests.

Acceptance: valid transfers durable across restart, bad signature/nonce/chain,
overspend/overflow/self-transfer/unknown account rejected, supply conserved, no
double application, altered payload rejected. No single-node shortcut may silently
enable non-consensus writes in cluster mode.

## Phase 3 — consensus (depends on Phase 2)

- [ ] Deterministic Raft core/clock/transport effect interfaces and simulation tests.
- [ ] Election/vote persistence, randomized timeouts, term handling and heartbeats.
- [ ] Durable append/conflict repair, per-peer progress, current-term commit rule.
- [ ] Ordered committed application, leader hints, check-quorum, no-op barrier.
- [ ] Recovery validation/replay, restart catch-up, storage error fail-stop.
- [ ] Checksummed snapshots, staged install, atomic publication, safe compaction.
- [ ] Five-process crash/partition/delay scenarios and history/root comparison.

Acceptance: no conflicting committed index, majority commits during two crashes,
minority cannot commit, committed receipts survive restart, state roots converge.
Include stale responses, divergent suffixes, and interrupted snapshot installation.

## Phase 4 — sequencer and performance (depends on Phase 3)

- [ ] Bounded queues, immutable verification workers, ordered validation overlay.
- [ ] Batch size/timeout, backpressure, cancellation and idempotent pending handles.
- [ ] C++ load generator with rate, concurrency, duration, delay/failure schedule.
- [ ] Durable-confirmation timestamps, resource metrics, JSON and CSV reports.
- [ ] Actual baseline and batch-size sweep with hardware/build/workload metadata.

Acceptance: correctness tests still pass under concurrency/sanitizers; reports
contain measured successful commitment and application, all failures, and latency.
No performance target is accepted based on RPC admission throughput.

## Phase 5 — proofs and settlement (depends on replicated ledger correctness)

- [ ] Binary transaction Merkle trees/proofs and fixed-registry account proofs.
- [ ] Deterministic before/after roots, retained batch data, independent proof CLI.
- [ ] Shared Solidity/C++ vectors, two-leaf state-root update implementation.
- [ ] Bounded one-transfer settlement contract and authenticated fraud verifier.
- [ ] Full Foundry positive/negative/fuzz suite and CI contract job.
- [ ] Anvil deployment and publisher with durable retry/receipt tracking.
- [ ] DA retrieval and challenge-window retention tests.

Acceptance: modified tx/state/proof fails, genuine invalid supported transfer or
post-root can be challenged, false challenges fail, challenged batches cannot
finalize, valid batch finalizes only after expiry. Multi-transfer L1 settlement
remains unsupported until its execution-trace protocol is separately verified.

## Phase 6 — reliability and deployment (depends on Phases 3–5)

- [ ] Automated kill/restart/partition/delay schedules and invariant checker.
- [ ] Full Prometheus transaction/Raft/storage metrics and Grafana dashboard.
- [ ] Five-node Docker fault suite with Anvil/Prometheus/Grafana/loadgen profiles.
- [ ] StatefulSet/PVC Kubernetes deployment after Compose is proven working.
- [ ] CI integration/fault/Foundry gates and reproducible demo.
- [ ] Complete limitations, measured results, recovery runbook and acceptance audit.

Acceptance: every user acceptance criterion has a reproducible passing command and
artifact. Kubernetes and smart-contract scaffolds alone are not completed phases.

## Verification and blockers (2026-10-09)

Further inspection found CMake 3.31.5, protoc 28.3, and cached GoogleTest 1.15.2
under `/opt/homebrew/Caskroom/miniforge/base`; these are not on the initial PATH.
They were used directly without installing or modifying system directories.

| Check | Result |
| --- | --- |
| CMake foundation-only configure and build with Apple Clang 21 / C++20 / warnings as errors | PASS |
| CTest `config_checks` | PASS |
| CTest `config_gtests` (four GoogleTest cases) | PASS |
| Separate ASan/UBSan CMake build and both configuration test executables | PASS |
| Direct compiler foundation configuration checks, ordinary and ASan/UBSan | PASS |
| Node main entry-point syntax compilation | PASS |
| All three protobuf schemas: C++ generation / descriptor validation | PASS |
| Compose, workflow and monitoring YAML parse; Grafana dashboard JSON parse | PASS |
| Shell syntax for all three scripts | PASS |
| HTTP runtime checks | BLOCKED: bind/listen returns Operation not permitted in sandbox |
| Full CMake node configure | BLOCKED: gRPCConfig.cmake unavailable; RocksDB also absent |
| gRPC/storage GoogleTests and five-process mesh | NOT RUN: full build unavailable and sockets denied |
| Docker build, Compose mesh, monitoring UI | NOT RUN: Docker unavailable |
| TSan, consensus/fault tests, Foundry, transaction benchmarks | NOT RUN; later phases pending |
| Requested `git init` | BLOCKED: `.git: Operation not permitted` (workspace read-only Git metadata) |
| Commit / branch / origin / push | NOT PERFORMED: initialization failed; GitHub DNS also fails |

The HTTP check fails on environmental socket access, not a demonstrated application
defect; HTTP behavior still requires runtime validation. Configuration-only tests
do not establish networking, RocksDB durability, consensus, or rollup correctness.
The first direct GoogleTest compiler invocation treated its headers as project
headers and hit a third-party Clang warning; the actual CMake imported target
correctly uses system includes and the resulting tests passed without disabling
project warnings.

Reproduce the available subset on this host:

```sh
cmake_bin=/opt/homebrew/Caskroom/miniforge/base/bin/cmake
ctest_bin=/opt/homebrew/Caskroom/miniforge/base/bin/ctest
gtest_dir=/opt/homebrew/Caskroom/miniforge/base/pkgs/gtest-1.15.2-h420ef59_0/lib/cmake/GTest
"$cmake_bin" -S . -B build-foundation-cmake -DROLLUPX_FOUNDATION_ONLY=ON -DGTest_DIR="$gtest_dir"
"$cmake_bin" --build build-foundation-cmake --parallel 2
"$ctest_bin" --test-dir build-foundation-cmake --output-on-failure -R config
```

Next required action: in an environment with dependencies, socket access, writable
Git metadata and network, run the full build/CTest and Compose commands in
docs/development.md, fix any integration failures, commit/push to the requested
repository, and only then begin Phase 2. No transaction, Raft, Solidity, Kubernetes,
or performance milestone has been marked complete.
