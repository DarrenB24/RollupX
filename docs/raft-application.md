# Committed Raft application

## Status

The application planner, recovery audit, and signed-transfer five-node simulation
run in the offline core build. `raft::Store` implements the corresponding RocksDB
writes and recovery path as source. Its database and SIGKILL tests have not run on
the current host because RocksDB is unavailable. The gRPC node still operates in
networking-only mode; this is not yet a durably replicated network ledger.

## Transfer commands

A nonempty transfer command uses the following concatenation. Integer fields are
unsigned big-endian; transaction envelopes use the canonical encoding in
[transaction-model.md](transaction-model.md).

| Field | Bytes |
| --- | --- |
| ASCII `RXC1` | 4 |
| Transaction count, 1–1,000 | 4 |
| Signed transaction envelopes, in execution order | 174 each |
| Keccak-256 checksum of preceding bytes | 32 |

The command is 214–174,040 bytes. Decoding bounds length/count, checks exact length
and checksum, and authenticates every envelope, including transaction identity,
canonical payload, recovered sender, and signature. A valid checksum does not
authorize a malformed transaction. The checksum is not a Merkle root.

The command has no embedded index: its position is assigned by Raft. The older
standalone ledger `RXB1` batch record is not accepted here. An empty Raft command
is a protocol no-op and cannot be submitted as a transfer batch.

Signature authentication does not imply execution success. Chain ID, account
membership, nonce, balance, self-transfer, and arithmetic checks run sequentially
against the preceding committed state. Invalid state transitions produce durable
rejection receipts without changing balances or nonces. Future leader admission
must check those constraints before sequencing as well.

## Storage and application boundary

Ledger-enabled construction binds the manifest's genesis digest to the complete
encoded genesis record and atomically initializes `meta/identity`, `raft/state`,
`ledger/genesis`, and `ledger/state`. Accounts use the existing fixed 16-slot
registry. Core-only and ledger-enabled directories cannot be silently converted;
opening with a different mode or genesis fails.

```cpp
raft::Store store(directory, manifest, genesis);
raft::Core core(config, store.load(),
  [&store](const raft::State& next) { store.persist(next); });
// One owner drives Core events and dispatches returned protocol messages.
// After successful events, apply through the current durable commit index:
store.apply_committed();
```

The store and core must have identical node/membership configuration. Store
callbacks must not reenter the store. Its mutex serializes persistence, application,
and queries; the protocol core itself requires a single owner. The runtime driver
must stop the node if application fails, even if consensus persistence succeeded.

`persist` synchronously records term/vote/log/commit before returning. It also
authenticates new or replaced nonempty commands in ledger mode. Commitment and
application are distinct durable operations. `apply_committed` processes only
`applied_index + 1` through the durable commit index, in order. Each entry uses one
WAL-enabled, synchronous RocksDB WriteBatch containing:

* The complete account state and new applied index under `ledger/state`.
* New receipts under `receipt/<transaction-id>`, including original log index,
  transaction offset, and result.

A no-op advances the applied marker without creating a receipt or transfer batch.
Duplicates within/across batches retain the first receipt and have no second state
effect. Both success and rejection receipts participate in deduplication. Batch
queries use Raft log positions, so no-op positions intentionally return no batch.

The method reports progress only after synchronous writes. Exceptions during
application poison the handle until reopen, including ambiguous post-write errors.
Account/receipt queries expose applied state; they do not provide ReadIndex or
linearizable follower reads. Client confirmation must wait for the durable receipt,
not merely an append acknowledgment or commit-index observation.

## Recovery protocol

Before exposing a reopened ledger-enabled store:

1. Check exact manifest/genesis, consensus record, and application record.
2. Authenticate every retained nonempty command, including uncommitted entries.
3. Scan receipt records and verify key/identity, applied/committed bounds, and
   origin transaction at the claimed position; no-op origins are invalid.
4. Replay the already-applied prefix from genesis without writing. Compare every
   expected receipt, including its first occurrence, and the complete final state.
   Authenticate the pending committed suffix before starting any replay writes.
5. Apply only the remaining committed entries with the atomic batches above.

If a crash occurs after commit but before application, step 5 performs the missing
effects. If it occurs after an application WriteBatch, its receipt/account/applied
marker are recovered together and that entry is not executed again. A crash during
this replay follows the same protocol on the next restart. Missing receipts,
forged results, changed account state, and receipts pointing to later duplicates
fail the audit instead of silently suppressing execution.

The audit retains one batch at a time and uses persisted receipt lookups. The log
is still bounded and retained in full; snapshot-aware auditing and compaction are
pending. Checksums detect corruption, not malicious rollback of a whole database.

## Verification

The core-tier tests exercise command authentication, no-ops, rejection/duplicate
semantics, every applied boundary of a small history, forged receipts, state
corruption, and genesis binding. A simulated leader crash/restart applies signed
commands independently on all replicas and compares resulting state.

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
```

With real RocksDB installed, the following additionally builds the store and runs
database and process-kill tests:

```sh
cmake -S . -B build-ledger -DROLLUPX_LEDGER_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ledger --parallel 2
ctest --test-dir build-ledger --output-on-failure
```

Six application database tests cover commit/application separation, committed-only
replay, poisoned handles, core/store integration, mode/genesis mismatch, and invalid
command admission. Two application process tests kill workers after commit,
before/after transfer application, and repeatedly during startup replay. Workers
must terminate by SIGKILL at a requested hook within 15 seconds; missed hooks fail.
These are implemented tests awaiting execution, not proof of power-loss durability
or of interruption inside RocksDB's own WAL recovery.

The next acceptance gate is the real storage suite, followed by gRPC consensus
integration and five-process fault tests. Merkle commitments, snapshots, settlement,
sequencing workers, and throughput measurements remain pending.
