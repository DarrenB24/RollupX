# RollupX

An incremental C++20 optimistic-rollup research project: a five-node, crash-tolerant
replicated transfer ledger with Ethereum settlement as its eventual destination.

**Current milestone: replicated transfer ledger with durable public APIs.**
Five native processes can elect a leader, commit signed transfers, serve durable
receipts/balances/nonces, and recover after process crashes. Storage-level
snapshots support validated, atomic recovery; live snapshot transfer, compaction, performance measurement,
Merkle proofs and Ethereum settlement remain unimplemented. See
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for verification evidence and
[replicated-node.md](docs/replicated-node.md) for the API contract and demo.

Start with the [architecture](docs/architecture.md), [transaction model](docs/transaction-model.md),
[consensus protocol](docs/consensus.md), and [fraud-proof design](docs/fraud-proofs.md).
Build, tests, and demo instructions are in [development.md](docs/development.md).

## Implemented source

* C++20 CMake targets with strict warnings and ASan/UBSan or TSan options.
* Public ledger and internal peer/Raft protobuf schemas on separate gRPC listeners.
* Static membership validation, bounded peer probes, recent-peer status, and CLI.
* Exclusive RocksDB directory ownership and synchronously persisted identity.
* HTTP health/readiness and actual Prometheus transport metrics.
* GoogleTest configuration/storage/mesh tests and a five-process crash/restart script.
* Five-node Compose deployment with separate volumes, optional monitoring dashboard,
  and GitHub Actions build/test jobs.
* Ethereum-compatible Keccak, OpenSSL secp256k1 signing/recovery, strict canonical
  transactions, deterministic in-memory transfer execution, and an offline wallet.
* Standalone RocksDB ledger source with atomic state/receipts, startup audit/replay,
  a local query CLI, and verified real-database crash tests.
* Tested protobuf transaction authentication, redundant-field validation, bounded
  parsing, and an offline envelope/protobuf conversion CLI.
* Isolated Raft protocol core with persistence ordering, elections, log repair,
  majority commitment, check-quorum, and deterministic five-node fault simulations.
* Tested identity-bound Raft records and persistence transition guards; RocksDB
  Raft store and verified process-kill recovery tests.
* Tested committed-entry application and recovery audit, including no-op progress
  and idempotent receipts; atomic RocksDB application/replay source and crash tests.
* Raft Protobuf codec with bounded validation, configuration checks, correlated
  replies, and five-node serialized-message fault simulations.
* Shared named Raft identity records that bind persistent votes/logs and wire
  configuration to the same member mapping, with explicit legacy-format rejection.
* Single-owner Raft runtime with bounded admission, automatic ticking, cancellation,
  fail-stop handling, safe shutdown, and tested concurrent producers.
* Bounded peer dispatch and ingress with deadline/cancellation handling, plus a
  threaded five-replica partition/healing test using actual Protobuf serialization.
* Raft gRPC adapter and loopback-test source with a storage-independent build tier;
  verified native socket transport.
* Owner-thread transfer admission and bounded receipt confirmation, including
  concurrent retries, durable public queries, leader discovery, and five-process fault tests.

The native dependency gate passed in [main-branch CI](https://github.com/DarrenB24/RollupX/actions/runs/39860618296).
The new replicated-node integration is verified locally; see the plan for its
exact test coverage and remaining limits. CI results for the earlier commit do
not certify the new changes.

## Build and test

The offline transaction core is buildable independently with OpenSSL 3 and GoogleTest:

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
```

See [offline wallet and execution](docs/crypto-and-execution.md) for signing commands,
test vectors, and the distinction between reference execution and durable commitment.
The core build also runs [Raft simulation tests](docs/raft-core.md); these do not
replace real-database or five-process consensus tests.
It includes [transaction admission and confirmation](docs/transaction-admission.md)
tests that distinguish a local append, majority commitment, and an applied receipt.
It also tests the [threaded Raft runtime](docs/raft-runtime.md), including a separate
ThreadSanitizer check. The [Raft transport](docs/raft-transport.md) documents tested
peer scheduling and the native gRPC adapter. Use
`-DROLLUPX_TRANSPORT_ONLY=ON` to build its socket tests with gRPC but without RocksDB.
The next build tier, `-DROLLUPX_LEDGER_ONLY=ON`, adds real RocksDB without gRPC.
See [persistence and recovery](docs/persistence.md) for its protocol, CLI, and tests.
That build also includes the [Raft persistence suite](docs/raft-persistence.md).
See [committed application](docs/raft-application.md) for command encoding,
commit/application boundaries, recovery, and verification status.
With protobuf installed, `-DROLLUPX_WIRE_ONLY=ON` builds the tested transaction
adapter and `rollupx-wire` without RocksDB/gRPC. See [wire validation](docs/transaction-wire.md).
This tier also tests the [Raft message boundary](docs/raft-wire.md), including
serialized elections, transfers, leader restart, and partition recovery.

Install the native dependencies listed in [development.md](docs/development.md), then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For a full Docker build and five-node transport test:

```sh
bash scripts/compose-smoke.sh
```

For an interactive local cluster:

```sh
docker compose up --build -d --wait
docker compose exec node1 rollupx-cli status 127.0.0.1:50051
curl --fail http://127.0.0.1:9101/readyz
docker compose down
```

Expected after convergence: every node reports `NETWORKING_ONLY`, four reachable
peers, and `transport_ready=true`. There is no elected leader at this milestone.
`/readyz` measures full-mesh transport connectivity, not ledger quorum readiness.
Stopping Compose preserves node volumes. The smoke script exercises process
restart and peer reconnection, not committed-transaction recovery.

## Replicated ledger and next milestones

```sh
bash scripts/replicated-smoke.sh ./build/rollupx-node ./build/rollupx-cli ./build/rollupx-wallet
```

This verifies signed transfers, retry deduplication, majority progress after leader
and follower crashes, catch-up, and full-cluster crash recovery. Enable persistent
ledger mode with `--genesis` and `--public-peers`; see the
[replicated node guide](docs/replicated-node.md). Balance/nonce reads are local
snapshots with applied indices; follower reads may lag.

The [snapshot store](docs/raft-snapshots.md) can create audited checkpoints and
resume staged installation before runtime startup. It retains signed history.
Next: live snapshot transfer/compaction and broader process partition/delay tests, then
bounded batching and measured performance, Merkle proofs, fraud challenges, Anvil
settlement and deployment acceptance. No TPS or latency benchmark exists yet.
The log is currently bounded to 4096 entries/8 MiB.

The design intentionally begins with an immutable 16-account registry and a bounded
single-transfer settlement witness. Multi-transfer L1 settlement requires a later
execution-trace protocol. Transport is currently local/private plaintext with no
cryptographic peer authentication. See the architecture for all trust assumptions.

Requested upstream: [DarrenB24/RollupX](https://github.com/DarrenB24/RollupX).
The repository now contains the user's initial commit and configured origin.
Agent commit/push outcomes and any remaining permission/network blockers are recorded
in IMPLEMENTATION_PLAN.md.

Raft provides crash fault tolerance within the documented model, not Byzantine fault tolerance. This is an
educational prototype, not production-secure Ethereum settlement or an EVM rollup.
