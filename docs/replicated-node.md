# Replicated ledger node

`rollupx-node` enables the replicated ledger when given both `--genesis FILE` and
`--public-peers id=host:port,...`. Without them it retains the explicit
`NETWORKING_ONLY` transport diagnostic mode. Use a fresh data directory when
switching modes: existing standalone, networking-only, and replicated databases
are not silently converted.

## Run and verify

Build the full native targets as described in [development.md](development.md).
The reproducible five-process demonstration generates temporary wallets/genesis,
starts five independent databases and gRPC listeners, transfers funds, checks
identical receipts/balances/nonces, kills the leader and a follower, commits with
the surviving majority, catches up the restarted nodes, then kills/restarts all
five processes and checks the same durable receipts again:

```sh
bash scripts/replicated-smoke.sh ./build/rollupx-node ./build/rollupx-cli ./build/rollupx-wallet
```

It uses ports 25010–25052; set `ROLLUPX_REPLICATED_BASE_PORT` for a different range.
CTest registers it as `five_process_ledger`. `replicated_node_tests` separately
covers concurrent first submissions, follower retries, loss of quorum, invalid
admission, missing queries, unsupported proofs, and incompatible database reuse.

Membership requires an odd count of 3–31 nodes; a single-member instance is not
supported. Use the five-process script above for a complete runnable example.

For five members, provide every node with identical genesis contents, all five
internal `--peers` routes and public `--public-peers` routes, plus its own ID,
listeners and data directory. Public routes are advertised client destinations;
they must be reachable by clients. The CLI exposes leader discovery explicitly
and does not automatically forward or retry a submission.

## Identity, ownership and shutdown

Member names are sorted once into IDs 1..N. The named manifest binds cluster,
local identity, membership, and canonical genesis digest. One `raft::Store`
opens exclusively, validates its manifest and retained history, and replays any
committed suffix before listeners start. The exact same manifest configures the
wire codec and peer transport. Endpoint addresses are routing configuration;
changing an address does not change the persisted member identity.

One runtime owner invokes synchronous persistence, deterministic application, and
leader admission. The store's locked reads supply the public query service and
durable receipt bridge. There is no separate direct-write ledger in this mode.
Election timeouts are randomized per process between 600 and 1200 ms; heartbeat
interval is 50 ms. The initial sequencer permits one outstanding transfer.

Shutdown closes submission/ingress waits, requests runtime/dispatcher stop,
shuts down and waits for public/internal handlers, then joins runtime and peer
workers. The store outlives all of them. Runtime, dispatcher, submission, or query
storage failures withdraw readiness and make the executable exit nonzero.

## API contract

- `SubmitTransaction` authenticates canonical fields and signature. Success
  returns only a durably applied receipt. Admission rejection is
  `FAILED_PRECONDITION` without a durable receipt; saturation/log capacity is
  `RESOURCE_EXHAUSTED`; a follower without the receipt returns `UNAVAILABLE`.
  A previously applied transaction can be retried on any replica with its receipt.
- Cancellation, deadline expiry, or leadership loss can occur after append.
  These errors never imply rollback. Query/retry the identical signed transaction
  by its ID; do not invent a new nonce to resolve an ambiguous result.
- `GetTransaction` returns locally durable receipts. `NOT_FOUND` can mean the
  transaction has not reached this replica; it does not prove global absence.
- `GetBalance` and `GetAccountNonce` read one atomic local state snapshot per RPC,
  with its `applied_index`. They are not linearizable reads. Two separate calls
  can observe different indices. Followers can lag; callers must compare progress
  against their required receipt index.
- `GetBatch` and `GetBlock` are aliases, indexed by the applied Raft log index.
  No-op indices return `NOT_FOUND`. Roots and commitments are empty because
  Merkle proofs/settlement are not implemented. `GetTransactionProof` and
  `InstallSnapshot` return `UNIMPLEMENTED`.
- `GetLeader` returns the currently known leader's ID, public endpoint and term.
  It is a routing hint, not a promise that the leader remains available.
- Status reports `REPLICATED_LEDGER`, role, term, leader and commit/applied indices.
  `ledger_ready`, `/readyz` and `rollupx-cli ready` require a healthy leader with
  its current-term barrier applied. Followers are live but are not write-ready.
  `transport_ready` remains the separate full-mesh probe observation. A majority
  can commit with two peers down even when `transport_ready` is false.

Readiness is a point-in-time observation and can change immediately. Check-quorum
withdraws leadership after loss of contact; it cannot predict a new partition.
The HTTP metrics include consensus-enabled, leader readiness, term and
commit/application indices, in addition to transport probes.

## Remaining limits

The log remains bounded to 4096 entries/8 MiB. The [snapshot store](raft-snapshots.md)
supports recovery before runtime startup; live transfer and compaction are pending.
The node rejects excess work rather than silently discarding history. This
milestone has no batching pipeline, throughput benchmark, Merkle commitments,
Ethereum settlement, or Byzantine protection. Internal/public gRPC is plaintext
for private development use. The existing Compose setup remains the transport
smoke deployment; replicated Compose and broader process partition/delay testing
are subsequent work. System gRPC/RocksDB libraries are not instrumented by the
project's sanitizer switches.
