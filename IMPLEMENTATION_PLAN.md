# RollupX implementation plan

## Execution rules and current environment

Work in phase order; compile and run relevant tests before promoting a milestone.
Never return placeholder success for absent consensus, execution, or settlement.
This plan is the status ledger; design documents describe future behavior.

### Current next steps (2026-10-22)

The latest verified increment is the owner-thread transfer admission and durable
receipt confirmation bridge, including threaded five-replica submission/retries.
The full rollup is not complete. Remaining work, in dependency order:

1. **Unblock native integration:** build/run gRPC transport and RocksDB ledger/Raft
   suites on a dependency-equipped host. Verify sync-write recovery and actual
   sockets; CI configuration alone is not evidence. Current host cannot resolve
   GitHub for downloads and lacks both dependency configurations.
2. **Compose the replicated node:** use one audited Store/Runtime/transport identity,
   install the admission policy and confirmation bridge, expose durable transaction,
   balance/nonce/batch and leader queries, and wire readiness/failure shutdown.
   Acceptance: signed client transfer produces an applied receipt on the five-node
   cluster with identical balances/nonces and idempotent retries.
3. **Complete consensus recovery:** run real five-process leader/follower kill,
   restart, partition, delayed-message and duplicate-submission tests; implement
   snapshot creation/install/compaction with crash-safe publication. Acceptance:
   committed prefixes never conflict and committed transfers survive recovery.
4. **Build and measure the pipeline:** bounded mempool, validation workers, batching,
   configurable C++ load generator, committed TPS and applied-receipt latency
   reports, replication/storage metrics, and batching comparisons. Optimize only
   after native replicated-ledger tests pass.
5. **Add verifiable settlement:** transaction/account Merkle trees and proof APIs,
   state/batch commitments, data availability, Solidity fraud challenges, and
   Foundry/Anvil end-to-end tests. No unsupported-accusation challenge shortcut.
6. **Finish deployment and acceptance:** verified five-node Compose fault demos,
   Anvil/monitoring services, Grafana dashboard coverage, then Kubernetes with
   persistent volumes. Complete CI and document measured performance/limitations.

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
- [x] Authenticated Raft transfer commands, ordered application planner, no-op progress,
      first-receipt audit, and independent simulated replica application.
- [x] Atomic RocksDB application/replay and database/process-kill test source
      (real storage compilation and execution still blocked).
- [x] Bounded generated-Protobuf Raft codec, configuration/route checks, original
      request correlation, and serialized five-node application/fault simulations.
- [x] Versioned named membership mapping bound to Raft records and wire identity;
      real-database reopen tests implemented, awaiting execution.
- [x] Bounded single-owner runtime, automatic ticks, cancellation, weak callback
      handles, fail-stop behavior, shutdown/join, and concurrent producer tests.
- [x] Bounded peer workers/ingress, deadline/cancellation handling, and threaded
      five-replica serialized transfer/partition tests (memory-backed persistence).
- [x] gRPC service/client adapter and loopback test source; transport-only CMake/CI tier.
- [x] Owner-thread single-transfer admission, bounded durable-receipt confirmation,
      and concurrent retry tests with five threaded replicas (memory-backed).
- [ ] Compile/run actual gRPC adapter; node-driver and public ledger integration.
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

## Continuation: committed Raft application (2026-10-11)

The user committed the preceding work, including the initial application bridge,
as `96a5bd6` (`phase 4`). That commit title is not phase acceptance: networked
consensus, real storage verification, and performance work remain pending.

The bridge introduces index-independent `RXC1` authenticated transfer commands,
sequential committed-entry planning, no-op applied markers, and a streaming replay
audit. Receipts retain their first log/transaction position across retries. Genesis
is bound to the Raft manifest. Ledger-enabled `raft::Store` persists accounts,
receipts, and progress atomically, audits startup state, and applies only the
remaining committed suffix. Core-only directories cannot be silently upgraded.
The five-node signed-transfer simulation now uses this shared planner/audit.

Verification in this continuation found and fixed duplicate GoogleTest exception
macro labels in two database test files, and guarded an optional receipt before
dereference. Added three further offline adversarial tests: malformed commands and
signatures with recomputed checksums, receipts moved to later duplicate positions,
and forged success/rejection results. The command/application suite now has eleven
tests. See docs/raft-application.md for encoding, recovery, and integration rules.

Local verification: all four core-tier CTest entries passed in Debug and ASan/UBSan,
including 76 GoogleTests (35 core and 41 Raft/record/application), configuration
checks, and wallet smoke. No skipped tests or sanitizer reports. The application
crash worker, process-test harness, and replicated-ledger integration test source
passed strict syntax-only compilation; this does not establish linking or storage
behavior. The RocksDB adapter and RocksDB-header-dependent tests remain uncompiled.

The ledger-only CMake configure was retried and still fails at
`find_package(RocksDB)`: `RocksDBConfig.cmake` is missing. Six application database
tests and two application SIGKILL tests are wired into the ledger/full CI builds
alongside the existing storage suites, but have not run here. Remote CI results
have not been observed. No disk-crash durability, network commitment, performance,
state-root, or settlement milestone is claimed.

Next: execute the real ledger/Raft storage suites, then connect correlated Raft
messages and a bounded single-owner event loop to gRPC. Run five-process transfer,
crash/restart, and partition tests before snapshot/compaction and performance work.

The requested commit/push was attempted for this continuation. Staging failed with
`.git/index.lock: Operation not permitted`; Git metadata is read-only in this
workspace. Neither commit nor push ran. Changes remain saved locally above
`96a5bd6`. `git diff --check` passed.

## Continuation: Raft Protobuf integration boundary (2026-10-11)

The preceding application verification changes were committed by the user as
`e21d2de` (`phase 4.1`), and this continuation started from a clean tree. A fresh
ledger-only configure still fails because RocksDB is absent; inspection also found
no usable gRPC installation. The existing Protobuf/protoc installation remains
available, so this step implements and verifies the transport-independent message
boundary without enabling unverified network writes.

Added `rollupx_raft_wire` and mandatory `RaftContext` fields for vote/append requests
and replies. The codec binds cluster, sorted named membership, and genesis through
a domain-separated configuration digest. It verifies sender/recipient direction,
request term and append sequence against the retained original call, bounds raw
and decoded messages, rejects unknown fields at each nesting level, and validates
contiguous log positions, terms, counts, payload size, and response hints. Higher
term rejection remains legal; a newer-term response cannot grant an old vote or
claim successful replication for an old append. Snapshot transport remains absent.

Added thirteen GoogleTests: eleven codec/validation cases and two deterministic
five-node serialized-message scenarios. Every simulated link uses actual generated
Protobuf serialization/parsing. The scenarios replicate and apply signed transfers,
restart a leader, retry idempotently, partition a leader with one follower, reject
minority commitment, repair the divergent suffix, and compare final balances,
nonces, and receipts. Every delivered event checks committed-prefix consistency.
Their persistence is explicitly memory-backed; these are not process/socket tests.

Verification: Debug and ASan/UBSan wire builds both passed all seven CTest entries,
including 98 GoogleTests (35 core, 41 Raft/application, nine transaction-wire, and
thirteen Raft-wire), configuration checks, wallet smoke, and wire CLI smoke. No
skips or sanitizer failures were reported. The first compile caught an incorrect
test-fixture call; it was corrected before the passing builds. `git diff --check`
passes. Existing protobuf/full CI jobs include the new target automatically; no
remote CI result has been observed. See docs/raft-wire.md for the full contract.

Next gates: execute real RocksDB storage suites; persist/verify the named-to-numeric
member mapping (the current numeric Raft manifest alone does not bind names); add
a bounded single-owner runtime with gRPC clients/services and cancellation/shutdown
handling; then run real five-process replication/fault tests. This step does not
complete Phase 3 or authorize moving to performance/settlement milestones.

Commit/push was attempted. `git add` failed with `.git/index.lock: Operation not
permitted`; no commit or push ran. The source, tests, schema, and documentation
remain saved in the working tree above `e21d2de`.

## Continuation: shared named Raft identity (2026-10-11)

The Protobuf-boundary changes above remain in the working tree. This continuation
closes the named-membership identity gap before the runtime owner/transport work.

Added `RXM2` manifests with sorted unique member names bound to numeric IDs 1..N.
The exact manifest remains part of the Raft state-record hash, so renamed members
cannot reinterpret previously persisted votes/log entries. Legacy numeric-only
`RXM1` encoding is preserved, with no automatic conversion in either direction.
Existing store identity comparison rejects renamed/downgraded/upgraded identities
before application replay. Shared manifest construction/validation and configuration
digest calculation now serve both storage and the Protobuf codec. The codec accepts
a named manifest directly, exposes it for store/Core initialization, and rejects
legacy identities. The previous wire digest preimage is unchanged and tested.

Added six offline record tests and one wire identity/recovery test. Coverage includes
maximum-length manifests, every truncated prefix of a named record, malformed names
with recomputed checksums, nonpositional IDs, format separation, member renaming,
and recovered transport identity. Both serialized five-node scenarios now reopen
their wire codec from an encoded named manifest and use it to bind every persisted
state image. Two new real-database test cases exercise rename/downgrade/upgrade
rejection with committed ledger recovery; their execution remains pending.

The first run exposed a miscalculated maximum-record-size constant; the bound is
now computed from field sizes and its 4,326-byte maximum test passes. Debug and
ASan/UBSan wire builds passed all seven CTest entries, including 105 GoogleTests
(35 core, 47 Raft/records/application, nine transaction-wire, fourteen Raft-wire),
configuration checks, and both CLI smoke tests. No skips or sanitizer reports.
The updated real-database test source passed strict syntax checking. Ledger-only
configuration was retried and still fails on missing `RocksDBConfig.cmake`; actual
database persistence has not been verified. `git diff --check` passes.

Next: run real storage suites, implement the bounded single-owner runtime with
gRPC clients/services and shutdown/cancellation, and execute five-process fault
tests. The new identity format does not complete networked consensus, snapshots,
performance, or settlement milestones. See docs/raft-persistence.md and
docs/raft-wire.md for compatibility and integration requirements.

The core-only build was also rebuilt after the bound correction; all four CTest
entries passed (82 GoogleTests plus configuration and wallet smoke). Staging the
combined pending changes for the requested commit/push was attempted, but `git add` failed
with `.git/index.lock: Operation not permitted`. No commit or push ran; all changes
remain saved locally above `e21d2de`.

## Continuation: bounded single-owner runtime (2026-10-11)

The wire and named-identity work remains pending in the working tree. Added a
thread-owning runtime around Core with count/byte admission bounds, automatic
steady-clock ticks, injected test clocks, future-based event completion, queued
cancellation, and weak callback handles. The active event remains charged to the
budget. Reserved vector/string capacity is accounted for, and an overlarge event
cannot bypass limits by having a small logical payload. Returned proposal indices
mean local append only; receipt-based client confirmation remains a separate step.

Persistence, application, and outbound dispatch run on one owner. Unary replies
are returned only after required persistence/application. The application callback
must reach the exact commit marker, including on startup. Explicit leader rejection
is nonfatal; other storage/application/driver/protocol failures stop admission,
complete active/queued futures, and retain the original exception for diagnosis.
Dispatch backpressure is counted as message loss, never as acknowledgment. Stop
finishes active work, cancels queued work, joins once even under concurrent calls,
and makes late callback handles reject work instead of touching a replacement node.

Added seventeen real-thread GoogleTests using synchronization gates and deadlines,
including count/byte saturation, 320 concurrent producer operations, reply ordering,
ambiguous persistence/application failures, startup replay, idempotent signed
transfers through the shared planner, weak-handle lifetime, clock regression,
automatic election ticks, concurrent stop, and owner self-join rejection. Persistence
in this suite is memory-backed. Added a real RocksDB runtime/reopen test to the
ledger tier; its source passes strict syntax checking, but it has not run.

Verification: all eight wire-tier CTest entries passed in Debug and ASan/UBSan,
including 122 GoogleTests (35 core, 47 Raft/application, 17 runtime, nine transaction
wire, fourteen Raft wire), configuration, wallet smoke, and wire CLI smoke. A
separate ThreadSanitizer build compiled and passed all seventeen runtime tests with
no reported races. No tests were skipped. CI now includes a dedicated runtime TSan
job in addition to the existing normal/ASan jobs; remote results remain unobserved.
CI YAML parsing and `git diff --check` passed. See docs/raft-runtime.md for ownership,
cancellation, callback, and shutdown contracts.

Next: verify the real storage suites, implement bounded gRPC request dispatch and
service handlers using the runtime/wire codec, connect durable public query and
submission APIs, and run five-process fault tests. The networking-only node is
unchanged; this step does not claim networked Raft, disk-crash durability, snapshot
support, settlement, or performance targets.

Ledger-only configuration was retried and remains blocked by missing
`RocksDBConfig.cmake`. Commit/push was attempted for all pending work, but staging
failed with `.git/index.lock: Operation not permitted`. No commit or push ran;
changes remain saved locally above `e21d2de`.

## Continuation: bounded peer transport and ingress (2026-10-11)

Started from the user's clean `ee174f2` commit, which includes the earlier runtime
and wire work. Added one worker per remote member with one active and one pending
RPC each, nonblocking admission/backpressure, original-request correlation, local
deadline enforcement, weak runtime handoff, terminal-error diagnostics, and
cancellation-aware shutdown. Ingress bounds concurrent calls and wait duration,
validates messages before admission, cancels queued requests on abandoned waits,
and exposes successful replies only after owner persistence/application completes.

Added twelve real-thread tests, including queue saturation and independent peers,
late/malformed/failed responses, cancellation callback lock safety, expired handles,
storage failure, unfinished application, and queued request cancellation. A
five-runtime/twenty-worker test serializes actual Protobuf messages, partitions the
leader with one follower, verifies minority noncommitment, elects a majority leader,
executes signed transfers with retries, heals the partition, and checks identical
committed prefixes, balances/nonces, and at-most-once receipts. Its durable-state
images are memory-backed, so this does not establish actual disk-crash recovery.

Implemented gRPC client/service adapters with per-call deadlines/cancellation,
endpoint-map validation, explicit errors, message/server resource limits, and four
real-loopback test cases. Snapshot RPCs remain explicitly unimplemented. The new
`ROLLUPX_TRANSPORT_ONLY` build tier isolates gRPC from RocksDB. CI includes normal
and ASan/UBSan socket jobs and expands the TSan job to runtime plus portable
transport. These remote jobs have not been observed running.

Verification: Debug and ASan/UBSan pass all nine wire-tier CTest entries, containing
134 GoogleTests (35 core, 47 Raft/application, 17 runtime, nine transaction wire,
fourteen Raft wire, twelve transport), configuration checks, and both CLI smoke
tests. The separate ThreadSanitizer build passes all 29 runtime/transport cases
with no reported races. No tests were skipped. Transport-only configuration was
attempted and fails on missing `gRPCConfig.cmake`; the gRPC adapter and its socket
tests remain uncompiled/unexecuted locally. See docs/raft-transport.md for limits,
ownership, cancellation semantics, build instructions, and the integration contract.

Next gates: install/verify real gRPC and RocksDB builds, compose the durable node
driver and public transaction/query APIs, then execute five-process failure tests.
Snapshot installation/compaction remains pending. The existing networking-only
node does not yet sequence transfers; no performance or settlement completion is
claimed by this step.

CI YAML parsing and `git diff --check` passed. The requested commit/push was
attempted, but `git add` failed with `.git/index.lock: Operation not permitted`.
No commit or push ran; the implementation and verification record remain saved
in the working tree above `ee174f2`.

## Continuation: guarded submission and durable confirmation (2026-10-22)

Started from clean commit `9c23600`, containing the previous transport increment.
Rechecked dependency availability: transport-only and ledger-only CMake configure
fail on missing `gRPCConfig.cmake` and `RocksDBConfig.cmake`; GitHub download lookup
fails DNS resolution. Native integration remains blocked, so this step implements
the independently testable admission/confirmation boundary needed by the node API.

Runtime now supports an owner-only proposal policy and weak-handle status reads.
An explicit policy result rejects admission without writing a log entry. Any
policy exception fails the runtime, including the Core admission-exception type;
this prevents a storage error from masquerading as an ordinary rejection.
Replicated entries bypass leader admission and retain deterministic application.

The ledger policy authenticates a single-transfer command and checks applied
progress, existing receipts, chain/account/nonce/balance/overflow rules, outstanding
log work, and log capacity before append. It allows only one outstanding command
and never publishes speculative account changes. This is a conservative initial
admission strategy, not the Phase 4 mempool/batching implementation.

The bounded submission bridge authenticates cached retries, waits for actual
application receipts, cancels abandoned queued proposals, reports ambiguous
deadline/leadership-loss outcomes, and fails closed on receipt-reader errors.
Admission rejection has no durable receipt; a previously committed rejection
returns its original durable receipt. Timeout does not release an uncommitted log
slot or imply rollback. Public gRPC submission/query wiring remains pending.

Added twelve submission tests, three runtime policy/lifetime tests, and one
five-runtime concurrent-submission/retry test. Coverage includes majority commit
with blocked application, late commitment after timeout, queued cancellation,
conflicting/duplicate requests, follower receipt retries, invalid signature IDs,
durable rejection results, bounded calls, and storage-error propagation. Added a
real RocksDB replay/reopen submission test; its strict C++20 syntax check passes,
but it cannot execute without RocksDB and is not counted as a passing runtime test.

Verification: all ten wire-tier CTest entries pass in Debug and ASan/UBSan, including
150 GoogleTests (35 core, 47 Raft/application, 20 runtime, 12 submission, nine
transaction wire, fourteen Raft wire, thirteen transport), configuration checks,
and both CLI smoke tests. The separate ThreadSanitizer build passes all 45
runtime/submission/transport cases without reported races. No tests skipped.
CI's TSan job now includes submission tests. See docs/transaction-admission.md for
the callback durability/lifetime contract, result semantics, and remaining wiring.

CI YAML parsing and `git diff --check` passed. Commit/push was attempted, but
staging failed with `.git/index.lock: Operation not permitted`. No commit or push
ran; all changes remain saved locally above `9c23600`.

## CI repair: GCC control-flow warnings (2026-10-22)

Inspected GitHub Actions run `39851008411` for commit `c0ef73b` through the user's
open browser after the GitHub CLI failed to connect. Two compiler blockers were
confirmed directly in the job logs:

- `build-and-test (OFF)`, job `120076839671`: GCC reports
  `-Werror=misleading-indentation` at `raft_transport.cpp:26`, where a loop and an
  unconditional queue-counter reset shared a line.
- `offline-core (ON)`, job `120076839830`: the same error at
  `ledger_records_test.cpp:87`, where conditional and unconditional returns shared
  a line. This prevents tests from starting; dependency installation succeeded.

The run's Clang TSan job succeeded. The screenshot also shows cancelled matrix
siblings; cancelled jobs are not additional failing test cases. Other job logs
have not been individually verified.

Added explicit control-flow braces at both confirmed failures and similar
multi-statement guards in runtime/transport tests, the network readiness test, and
the standalone store read helper. No consensus/execution behavior or warning flags
were changed. CI now retains compiler/Compose logs as artifacts and disables
matrix fail-fast so every variant can finish and report its own errors. Explicit
Bash execution preserves pipeline failure status when build output is saved by tee.

Local verification after the changes: all ten wire-tier CTest entries pass in
Debug and ASan/UBSan (150 GoogleTests plus configuration and CLI smoke checks).
All 45 runtime/submission/transport cases pass under TSan. CI YAML/matrix settings
and `git diff --check` pass. GCC, gRPC, RocksDB, and Docker are not available for
local reproduction of the entire Ubuntu workflow; the corrected commit must run
in GitHub Actions before the workflow can be declared green. Native dependency
runtime acceptance gates remain open.

Commit/push was attempted, but `git add` failed with `.git/index.lock: Operation
not permitted`. No commit or push ran; repairs remain saved above `c0ef73b`.
