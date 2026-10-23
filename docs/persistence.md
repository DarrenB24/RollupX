# Standalone ledger persistence and recovery

## Status and build

The RocksDB adapter, standalone CLI, database recovery and process-kill tests
pass in native builds and the recorded main-branch CI. The standalone ledger
remains an explicit development tool with direct local commitment. Its manifest
is deliberately incompatible with the networking and replicated-node stores.
The [replicated node](replicated-node.md) uses `raft::Store` instead of this adapter.

On Ubuntu 24.04:

```sh
sudo apt-get install -y build-essential cmake ninja-build librocksdb-dev libssl-dev libgtest-dev
cmake -S . -B build-ledger -G Ninja -DROLLUPX_LEDGER_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ledger --parallel 2
ctest --test-dir build-ledger --output-on-failure
```

The separate `standalone-ledger` CI job runs these commands normally and with
ASan/UBSan. It needs no gRPC or listening sockets. The full build includes the
same tests. Missing RocksDB fails configuration; there is no fallback persistence
implementation or simulated passing database test. Native CI execution is recorded
in IMPLEMENTATION_PLAN.md.

## CLI workflow

Generate accounts with `rollupx-wallet`, then supply a text genesis file:

```text
chain 31337
account <sender-address-without-brackets> 100
account <recipient-address-without-brackets> 0
```

There are at most 16 nonzero, unique addresses in fixed slot order. Initial nonces
are zero; total supply must fit uint64. The parser caps input at 4096 bytes and
rejects extra tokens, negative/overflowed numbers, and empty/duplicate registries.
No account is implicitly funded.

```sh
./build-ledger/rollupx-ledger --standalone ./data/dev ./genesis.txt init
./build-ledger/rollupx-ledger --standalone ./data/dev ./genesis.txt submit "$signed_transaction"
./build-ledger/rollupx-ledger --standalone ./data/dev ./genesis.txt balance "$sender_address"
./build-ledger/rollupx-ledger --standalone ./data/dev ./genesis.txt receipt "$transaction_id"
./build-ledger/rollupx-ledger --standalone ./data/dev ./genesis.txt batch 1
```

Only `init` permits creation. Every open checks the supplied genesis against the
persisted manifest. `balance` includes nonce and applied index. `batch` returns
exact signed transaction envelopes, including duplicates; it makes no Merkle-proof
or settlement claim. The automated CLI test is:

```sh
bash scripts/ledger-smoke.sh ./build-ledger/rollupx-ledger ./build-ledger/rollupx-wallet
```

It uses temporary keys/genesis/storage and separate processes for commands, so a
passing run requires real reopen/recovery between submission and queries.

## Persistent records

RocksDB exclusively owns one directory. A mutex serializes in-process updates and
queries; signature checks may run before locking, but state mutation is sequential.

| Key | Value |
| --- | --- |
| `meta/identity` | Explicit standalone format marker |
| `ledger/genesis` | `RXG1`, chain ID, 16 initial accounts, checksum |
| `ledger/commit` | `RXP1`, last locally committed index, checksum |
| `ledger/state` | `RXS1`, applied index, 16 current accounts, checksum |
| `log/` + 8-byte big-endian index | `RXB1`, index, count, signed envelopes, checksum |
| `receipt/` + transaction ID | `RXR1`, ID, first log index/offset, result, checksum |

Integers are unsigned big-endian. Accounts are address[20], balance[8], nonce[8].
Indices use 8 bytes; counts/offsets use 4. Batch size is 1–1000; transaction envelopes
are 174 bytes. Receipt result codes explicitly map to 0–8, independent of the C++
enum representation. Each typed record ends with Keccak-256 of its preceding bytes.
Wrong type/version, truncation, trailing bytes, bounds, and checksum failures reject
the record. These are storage checksums, **not Merkle state commitments**.

The bounded registry is stored in one aggregate value. Receipts are separately
keyed on disk. Application holds only one batch's new receipts in memory; a one-batch
cache bounds repeated log decoding. The full history remains on disk without
compaction until verified checkpoints are implemented.

## Commitment and application

1. Authenticate all inputs before committing anything. An invalid later signature
   prevents the entire batch entering the log.
2. In one `WriteBatch`, persist batch bytes and `ledger/commit`, with WAL enabled
   and `sync=true`.
3. Plan sequential execution from applied state. Existing IDs and within-batch
   duplicates reuse original receipts. Validly signed semantic failures produce
   deterministic rejection receipts without balance/nonce changes.
4. In a second synchronous `WriteBatch`, persist every new receipt and the complete
   new account state with its applied-index marker.
5. Publish cached state and return only after step 4. A direct retry of an existing
   ID returns the original result without another log entry.

A failure after commitment is ambiguous to callers: reopen and retry the same ID.
Operational commit/application failures poison the current handle until reopening;
pre-commit admission errors do not. Failure hooks are test-only callbacks executed
synchronously; they must not reenter the ledger.

This uses RocksDB's [synchronous writes and atomic WriteBatch](https://github.com/facebook/rocksdb/wiki/Basic-Operations).
Disks must honor synchronization; malicious storage is outside the guarantees.

## Startup and recovery

Validate identity/genesis, encodings, applied <= committed, registry, and supply.
Require a contiguous retained log with no entry beyond commit. Audit applied history
from genesis against persisted results/accounts, checking receipts' original log
positions. Missing committed data, missing receipts, or inconsistent state fail
closed rather than being silently reconstructed.

The shared streaming `RecoveryAudit` checks log order, authenticates the pending
suffix before replay, and compares every applied result with the stored receipt.
A receipt claiming an earlier occurrence must identify that transaction in the
original batch; it cannot suppress a transfer by pointing at an unrelated command.
The audit retains no growing receipt table. A failed or finished audit is closed
and cannot be reused to skip an error.

Then replay only `(applied, commit]` using the same atomic application path. Before
the application WriteBatch, the old state/marker remain; after it, new accounts,
receipts, and marker survive together. `snapshot()` is a coherent query result,
not a Raft snapshot or compaction operation.

The conservative audit scans history and its cost grows with retained data. No
three-second recovery target or throughput result is claimed. Checkpoints and
replicated commitment belong to Phase 3.

## Test coverage and remaining limits

Offline recovery tests enumerate every commit/application boundary in a three-batch
history with within-batch duplicates, cross-batch retries, and rejected transfers.
They check repeated recovery, corrupted state/results/positions, missing or reordered
history, malformed pending transactions, and forged earlier receipts. These tests
exercise the same audit/planner as the adapter using explicit state images; they
do not simulate or prove RocksDB's atomicity or durability.

The real RocksDB tests cover reopen/retries, concurrent duplicate callers, ordered
batches, durable rejection, malformed-batch atomicity, poisoned handles, identity
and ownership, missing log/receipt records, and inconsistent persisted state.

A separate executable is spawned for SIGKILL at `before_commit`, `after_commit`,
`before_apply`, and `after_apply`. Tests require the precise signal and bounded
termination time. Another case kills successive processes during replay before and
after the atomic application write. This tests application recovery; it does not
yet interrupt RocksDB's internal WAL recovery or emulate controller/power failure.

Native database/process tests and replicated public query integration now pass.
Snapshots, Merkle state commitments, and settlement remain pending. The standalone
command is never a nonconsensus write path in the five-node deployment.
Protobuf field validation is independently tested; see [transaction-wire.md](transaction-wire.md).
