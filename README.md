# RollupX

An incremental C++20 optimistic-rollup research project: a five-node, crash-tolerant
replicated transfer ledger with Ethereum settlement as its eventual destination.

**Current milestone: verified offline transaction core; networking integration
and durable execution pending.** This repository is not yet a working rollup.
Raft consensus, durable ledger execution, settlement, and performance results are pending. See
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for the implementation and
verification record. Unavailable APIs fail explicitly.

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
  a local query CLI, and real-database crash tests awaiting execution.
* Tested protobuf transaction authentication, redundant-field validation, bounded
  parsing, and an offline envelope/protobuf conversion CLI.

The gRPC/RocksDB source and five-node deployment **have not run on the initial
development host**: those dependencies are missing and socket binding is denied.
The CMake foundation build, configuration tests (including four GoogleTest cases),
protobuf schema generation, and YAML/JSON/shell checks have been verified. See the
plan for sanitizer results and exact remaining checks. CI configuration is not a
claim that CI has run.

## Build and test

The offline transaction core is buildable independently with OpenSSL 3 and GoogleTest:

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
```

See [offline wallet and execution](docs/crypto-and-execution.md) for signing commands,
test vectors, and the distinction between reference execution and durable commitment.
The next build tier, `-DROLLUPX_LEDGER_ONLY=ON`, adds real RocksDB without gRPC.
See [persistence and recovery](docs/persistence.md) for its protocol, CLI, and tests.
With protobuf installed, `-DROLLUPX_WIRE_ONLY=ON` builds the tested transaction
adapter and `rollupx-wire` without RocksDB/gRPC. See [wire validation](docs/transaction-wire.md).

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

## Next milestones and limitations

Phase 2's remaining gate is building and verifying the new RocksDB durability/query
path against real crashes, then connecting the tested protobuf adapter to the
replicated ledger and verifying network integration.
Phase 3 adds actual Raft and distributed failure recovery.
Only after those correctness gates pass do batching benchmarks, Merkle proofs,
Solidity fraud challenges, Anvil publication, and Kubernetes follow. No TPS or
latency result exists yet. Foundry tests and the load generator are not implemented.

The design intentionally begins with an immutable 16-account registry and a bounded
single-transfer settlement witness. Multi-transfer L1 settlement requires a later
execution-trace protocol. Transport is currently local/private plaintext with no
cryptographic peer authentication. See the architecture for all trust assumptions.

Requested upstream: [DarrenB24/RollupX](https://github.com/DarrenB24/RollupX).
The repository now contains the user's initial commit and configured origin.
Agent commit/push outcomes and any remaining permission/network blockers are recorded
in IMPLEMENTATION_PLAN.md.

Raft will provide crash fault tolerance, not Byzantine fault tolerance. This is an
educational prototype, not production-secure Ethereum settlement or an EVM rollup.
