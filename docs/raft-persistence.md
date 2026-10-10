# Raft persistence boundary

## Status and build

Versioned state records and transition validation are compiled and tested offline.
The real RocksDB `raft::Store` adapter and its database/process-kill tests are
implemented as source, but **have not been compiled against RocksDB or run on the current host**
because RocksDB is missing. The running node still does not expose Raft consensus.

The existing ledger build tier now builds both storage adapters and their tests:

```sh
sudo apt-get install -y build-essential cmake ninja-build librocksdb-dev libssl-dev libgtest-dev
cmake -S . -B build-ledger -DROLLUPX_LEDGER_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ledger --parallel 2
ctest --test-dir build-ledger --output-on-failure
# Only the real Raft database/restart/process-kill tests:
ctest --test-dir build-ledger --output-on-failure -R '^raft_store_tests$'
```

No listening socket is required. The standalone-ledger CI matrix runs this suite
with and without ASan/UBSan; remote CI has not run from this environment.
`ROLLUPX_CORE_ONLY` includes the record tests and protocol simulations, without
pretending the database tests passed.

## Identity and binary format

The store owns an exclusive RocksDB directory. Its manifest is incompatible with
both the networking-only node identity and standalone transfer-ledger formats.
Opening a different identity, a nonempty database with no manifest, or a database
with no required state record fails. `existing_only` never creates a database.
Initial creation atomically writes the manifest and empty state with WAL/sync.

`meta/identity` contains the following concatenation:

| Field | Bytes |
| --- | --- |
| ASCII `RXM1` | 4 |
| Cluster identifier length | 2 |
| Cluster identifier, ASCII letters/digits/`._-` | 1–128 |
| Local numeric node ID | 4 |
| Member count (odd, 3–31) | 1 |
| Sorted unique nonzero member IDs | 4 each |
| Genesis digest | 32 |
| Keccak-256 checksum of the preceding bytes | 32 |

The local ID must belong to the member set. The genesis digest is Keccak-256 of the
exact `encode_genesis` record, and cannot be zero. The future runtime adapter must
use the same stable name-to-numeric-ID mapping in its topology and Core config.

`raft/state` contains one complete persistent image:

| Field | Bytes |
| --- | --- |
| ASCII `RXF1` | 4 |
| Keccak-256 of the complete encoded manifest | 32 |
| Current term | 8 |
| Voted-for ID (zero means no vote) | 4 |
| Commit index | 8 |
| Entry count | 4 |
| Each entry: term, command length, command bytes | 8 + 4 + length |
| Keccak-256 checksum of the preceding bytes | 32 |

All integers are unsigned big-endian. Log index is entry position plus one. Empty
commands are Raft no-ops. Bounds match the core: 4,096 entries, 174,048 bytes per
command, and 8 MiB total command data. The maximum encoded record is 8,437,852 bytes.
Parsing bounds counts and lengths before reserving/copying data, rejects trailing
data, and verifies identity, checksum, vote membership, ordered terms, and commit
progress. Checksums detect corruption; they are not Merkle commitments, storage
authentication, or protection against intentional rollback of an entire database.

## Persistent transition rules

Before each write, the adapter verifies that term/commit never decrease, a recorded
vote cannot change or clear within the same term, and every committed log entry is
preserved. Different payloads at the same index/term are rejected. Removing a suffix
requires a conflicting replacement entry; snapshot/compaction deletion is not yet
supported. A higher term may clear the vote, and uncommitted conflicting suffixes
may be replaced. These checks are shared with the offline tests and simulations.

The adapter prepares the new state image and uses a RocksDB WriteBatch with WAL
enabled and `sync=true`. Term, vote, log, and commit become visible together.
Cached state changes only after the write succeeds. Invalid proposed transitions
fail before writing and leave the handle usable. Any exception during the write
boundary or its test hooks makes the handle unusable until reopen, including an
ambiguous error after a successful write. Repeated identical states are no-ops.

The integration pattern is:

```cpp
// manifest and config must identify the same local node and membership.
raft::Store store(directory, manifest);
raft::Core core(config, store.load(),
  [&store](const raft::State& next) { store.persist(next); });
// Dispatch returned envelopes only after Core methods return successfully.
```

The store must outlive the core. Callbacks and failure hooks must not reenter the
store. A mutex serializes store operations, but Core itself still requires one
event-loop owner. Startup loads and validates the entire image before creating
Core. No adapter silently reconstructs absent committed data or clears a vote.

This first store rewrites the bounded full image on each protocol mutation.
It favors an explicit atomic boundary over throughput; no benchmark or recovery
time target is claimed. Incremental log keys require a later, tested migration.

## Tests and remaining work

Nine offline record tests cover round trips, frozen endian offsets, mutations,
every truncated prefix, identity binding, valid-checksum malformed records, and
legal/illegal state transitions. Five-node simulations now encode/decode every
stored update and apply the same transition guard, including crash/restart and
partition histories. These still use memory for storage.

Six real database tests cover Core callback integration, vote retention, uncommitted
suffixes, exclusive ownership/identity, invalid updates, poisoned handles, suffix
replacement, and missing/corrupt/foreign records. One process test spawns a worker
and requires SIGKILL before and after each of five effects: term, vote, append,
commit, and suffix replacement. Each of the ten cases reopens twice and checks an
exact pre-write or post-write image; a persisted vote cannot be granted to another
candidate. Workers have explicit termination deadlines and missed hooks fail.

These real storage tests remain unverified. They test process interruption around
synchronous writes, not power loss or interruption inside RocksDB's WAL recovery.
Initialization crash points, checkpoints, snapshots, and compaction are pending.
The store does not contain accounts, receipts, or applied progress yet. Next verify
real storage, then implement the application bridge (including no-op markers) and
gRPC driver before claiming durable replicated transfers.
