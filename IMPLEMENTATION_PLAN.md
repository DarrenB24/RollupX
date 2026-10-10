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

## Phase 2 — transaction engine (offline core underway; durable acceptance pending)

On the user's continuation request, independently testable offline components were
implemented while Phase 1 networking remains blocked. This advances the transaction
core without enabling cluster writes or claiming either milestone fully accepted.

- [x] Golden canonical payload/ID/signature vectors and Ethereum Keccak verification.
- [x] OpenSSL secp256k1 signing, public-key recovery, strict low-s validation.
- [x] In-memory immutable funded genesis registry, checked arithmetic and nonce rules.
- [x] Deterministic reference execution with bounded ID deduplication and rejection results.
- [x] Offline wallet account generation, signing, and verification.
- [x] Persistence codecs, bounded application planner, recovery-state checks.
- [x] Shared streaming startup audit and exhaustive small-history recovery-boundary tests.
- [x] Standalone RocksDB ledger and query CLI source (not built against RocksDB yet).
- [x] Real RocksDB reopen/concurrent-duplicate/corruption and SIGKILL test source.
- [ ] RocksDB atomic batch execution, receipts, durable IDs, applied marker.
- [ ] Retry semantics, concurrent duplicate admission, deterministic rejection.
- [x] Generated-protobuf field validation and authenticated offline wire conversion.
- [ ] Durable network query APIs and replicated transaction admission.
- [ ] Process-kill hooks before/after storage effects and replay tests.

Acceptance: valid transfers durable across restart, bad signature/nonce/chain,
overspend/overflow/self-transfer/unknown account rejected, supply conserved, no
double application, altered payload rejected. No single-node shortcut may silently
enable non-consensus writes in cluster mode.

## Phase 3 — consensus (depends on Phase 2)

The isolated protocol core can be tested while earlier runtime gates are blocked.
Cluster integration and phase acceptance still depend on verified durable execution.

- [x] Deterministic Raft core/clock/transport effect interfaces and simulation tests.
- [x] Protocol-level elections, log repair, majority rule, no-op barrier and check-quorum.
- [x] Five-node simulated faults and independent committed signed-transfer execution.
- [x] Versioned identity-bound Raft records and shared persistence transition validation.
- [x] RocksDB Raft store and real-database/SIGKILL test source (runtime gate blocked).
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

Next integration gate: in an environment with dependencies and socket access, run
the full build/CTest and Compose commands in docs/development.md and fix any failures.
Phase 2 can continue as independently tested libraries, but durable acceptance
requires real RocksDB crash tests before Raft integration. No durable transaction,
Raft, Solidity, Kubernetes, or performance milestone has been marked complete.

## Continuation: offline cryptography and transaction execution

The user initialized Git between turns: the current baseline is `7b4f4c2` and origin
is `https://github.com/DarrenB24/RollupX.git`. GitHub DNS still fails from this sandbox.

Added `ROLLUPX_CORE_ONLY`, the crypto/execution libraries, `rollupx-wallet`, frozen
transaction vectors, 16 GoogleTest cases, and a subprocess wallet smoke test. The
normal full build and CI now include these tests; a separate offline-core CI job
does not depend on gRPC/RocksDB. Keccak is compared against OpenSSL 3.6.1 and frozen
Ethereum vectors. Signature recovery is checked against EIP-155 and EVP verification.
The deterministic tests cover 2000 seeded transitions plus replay, rejection,
arithmetic boundaries, bounded receipts, malformed envelopes, and concurrent
signature verification. This is correctness testing, not a throughput benchmark.

Final local verification: Debug CMake build and all three registered offline CTest
entries passed (configuration, 16 core GoogleTests, wallet subprocess smoke). A
separate ASan/UBSan build passed the same suite with no reported sanitizer failures.
The OpenSSL Keccak oracle ran successfully, with no skipped core tests. The expanded
test suite caught a transcription typo in the frozen signature fixture; it was
corrected against the independent reference before these passing runs. CI YAML,
fixture JSON/field lengths, and shell syntax also passed local checks. Full node,
RocksDB, Docker, and Raft tests remain unverified as recorded above.

Commit/push attempt for this continuation failed during `git add`:
`Unable to create .git/index.lock: Operation not permitted`. The existing repository
and origin can be read, but this agent still cannot update Git metadata. No new
commit was created and no push ran. Source changes remain in the working tree.

## Continuation: standalone ledger and recovery audit (2026-10-10)

The user committed the standalone ledger source as `f8d30ab` (commit title
`phase 3`). That title does not indicate completed Raft: Phase 3 remains pending.
This baseline includes the standalone RocksDB adapter/CLI, synchronous log/commit
and state/receipt WriteBatches, eight database tests, two process-kill tests, and
eleven offline record/planner tests. Its 27 core GoogleTests passed normal and
ASan/UBSan builds; actual database tests were not run.

This continuation extracts the adapter's startup audit into shared `RecoveryAudit`
code, adds eight offline tests, and adds a real-database regression for a forged
receipt targeting a pending transfer. The audit checks contiguous authenticated
history, receipt provenance, replayed results/accounts, and rejects reuse after
completion or error. Boundary tests enumerate committed/applied prefixes and
repeated restart images with duplicates and rejected transfers. These are logical
recovery checks, not substituted database or process-crash tests.

RocksDB-only configuration was retried and still fails because `RocksDBConfig.cmake`
is absent. GitHub dependency download still fails DNS lookup. Real RocksDB linking,
the nine database/two process-kill tests, and the ledger CLI smoke test remain
unverified; the next acceptance gate is their execution in the documented ledger
build environment. No networking, consensus, or performance milestone is promoted.

Local verification for this continuation: all 35 core GoogleTests passed, with no
skips, in both the normal Debug and ASan/UBSan builds. All three registered CTest
entries passed in each build (configuration checks, core tests, wallet smoke).
`git diff --check` passed. The new database regression is source-only until the
RocksDB build can run.

The requested commit/push was attempted again for these changes. `git add` failed:
`Unable to create .git/index.lock: Operation not permitted`. Git metadata remains
read-only in this environment; no commit was created and the chained push did not
run. The changes remain in the working tree on top of `f8d30ab`.

## Continuation: protobuf authentication boundary (2026-10-10)

The previous recovery changes remain in the working tree. This continuation adds
`rollupx_wire`: bounded raw protobuf parsing, exact binary field lengths, rejection
of unknown fields, comparison of every redundant field with the signed payload,
cryptographic authentication, and strict transaction-ID/account-address query keys.
`rollupx-wire encode/decode` converts authenticated wallet envelopes and protobuf
hex offline, without reporting submission or commitment.

The cached protoc/protobuf 28.3 C++ installation was available, enabling real
generated-message compilation without gRPC/RocksDB. `ROLLUPX_WIRE_ONLY` builds this
tier; CMake message generation is separated from gRPC stub generation. Nine new
GoogleTests exercise malformed/altered input, alternate legal protobuf encodings,
integer bounds, and query keys. A subprocess test generates temporary wallet keys,
round-trips signed data through the CLI, and rejects a conflicting unsigned amount.
See docs/transaction-wire.md for policy and reproduction commands.

Verification: Debug and ASan/UBSan wire builds both passed all five CTest entries:
configuration, 35 core GoogleTests, wallet smoke, nine protobuf GoogleTests, and
wire CLI smoke. No GoogleTests were skipped. Core-only and wire-only configurations
still succeed; workflow YAML, shell syntax, and `git diff --check` pass. A dedicated
CI matrix now builds this tier with/without sanitizers; remote CI has not run here.

The validator is ready for future public admission integration, but cluster mutation
APIs remain UNIMPLEMENTED. Real RocksDB durability/process-crash acceptance and
five-node gRPC runtime verification remain blocked by missing dependencies/socket
access. No Raft, state-root, settlement, or performance milestone is claimed.

Commit/push was retried for the combined pending changes. Staging again failed with
`.git/index.lock: Operation not permitted`; neither commit nor push ran. All source
and documentation changes remain saved locally.

## Continuation: isolated Raft protocol core (2026-10-10)

The previous changes are now committed by the user as `8b86cdd` (`phase 3.5`).
The tree was clean at the start of this continuation. gRPC/RocksDB are still absent,
and a fresh GitHub request still fails DNS resolution. Work therefore advances
the independently testable protocol core, without bypassing the durable-ledger
acceptance gate or enabling cluster writes.

Added a single-owner C++ Raft core with injected monotonic time and reproducible
randomized elections, persistent self/granted votes, term handling, per-peer log
progress, conflict repair, current-term majority commitment, a leadership no-op,
and recent-quorum proposal gating/step-down. Persistence is an explicit synchronous
callback; dependent messages are returned only after it succeeds. Storage failures
and allocation failures stop the core. Commands, logs, and per-peer in-flight
requests are bounded. No snapshots, real Raft store, gRPC driver, or durable
application bridge are implemented in this slice. See docs/raft-core.md.

Added 21 GoogleTests, including five-node scenarios with two crashed nodes, leader
restart, 2/3 partitions, divergent suffixes, split votes, and 20 seeded schedules
with loss, duplication, delay/reordering, and independent clock offsets. Every
delivered event checks same-term leader uniqueness and committed-prefix consistency.
A separate scenario independently executes real signed transfers from committed
logs and verifies that a retry after leadership change has only one balance/nonce
effect. Simulated memory recorders do not demonstrate disk crash durability.

The first test run caught an overly strict test assertion: after losing quorum,
an isolated former leader may time out as a follower and become a candidate again.
The requirement is that it cannot regain leadership or commit without quorum;
the corrected assertion and explicit quorum-step-down tests both pass.

Final verification: all four core-tier CTest entries pass in Debug and ASan/UBSan,
including 35 existing core cases, 21 Raft cases, configuration checks, and wallet
smoke. No tests were skipped or sanitizer failures reported. Workflow YAML and
`git diff --check` pass. CI now includes the Raft test target in its offline tier.
No real-time recovery target, throughput result, network consensus, or durable
replicated-ledger milestone is claimed.

Next: verify the real RocksDB ledger suite; implement a versioned Raft store and
crash-test its persistence callback; map core messages to gRPC with request-term/RPC
correlation; bridge committed transfer/no-op entries to atomic applied markers;
then run five real processes before snapshot/compaction and performance work.

The requested commit/push was attempted for this continuation. `git add` failed
with `.git/index.lock: Operation not permitted`. No new commit was created and
push did not run; all changes remain in the working tree above `8b86cdd`.

## Continuation: versioned Raft persistence (2026-10-10)

The preceding Raft core changes are still uncommitted in the working tree. Added
identity-bound manifest/state records, explicit big-endian encodings with Keccak
checksums, bounded decoding, and shared recovered-state validation. The persistence
guard rejects backward term/commit movement, changing a same-term vote, replacing
committed entries, and unsupported suffix deletion. It permits legal higher terms
and uncommitted conflict replacement. Every simulated persistence effect now passes
this guard and the actual encode/decode boundary.

Added `raft::Store`, a distinct RocksDB directory format implementing the core's
synchronous persistence callback. It writes term/vote/log/commit in one synchronous
WAL WriteBatch, checks staging/write errors, publishes cached state after success,
and requires reopen after ambiguous failures. Exact identity and exclusive ownership
are enforced. It does not yet store ledger effects or applied markers.

Added nine offline record tests, six real-database tests, and a process test with ten
SIGKILL cases: before/after term, vote, append, commit, and suffix replacement. The
process harness reopens twice and checks complete images and vote retention. Its
deadline bounds worker termination; a missed kill hook fails. These tests are wired
into the existing standalone-ledger CI matrix, not registered as passing placeholders.

Local verification: all 65 GoogleTests passed (35 execution/crypto/recovery + 30
Raft/record cases) in Debug and ASan/UBSan. All four core-tier CTest entries passed,
including configuration and wallet smoke, with no skipped tests or sanitizer reports.
The new process worker and parent harness passed syntax-only compilation. Workflow
YAML and `git diff --check` passed. RocksDB-only configuration was retried and failed
at `find_package(RocksDB)`: no `RocksDBConfig.cmake`. The actual adapter/database
tests have not compiled or run; neither process-kill durability nor network consensus
is claimed. See docs/raft-persistence.md for record layout and commands.

Next acceptance gate: execute both real RocksDB storage suites. Then bridge committed
Raft commands/no-ops to durable account/receipt/applied-index updates and connect the
core's correlated messages to gRPC. Snapshots/compaction, root commitments, settlement,
and benchmarks remain pending. Full-image persistence is intentionally bounded and
has no throughput claim.

Commit/push was attempted for the combined pending consensus/persistence work.
Staging failed with `.git/index.lock: Operation not permitted`; no commit or push
ran. Source and documentation changes remain saved locally above `8b86cdd`.
