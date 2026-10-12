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
The optional ledger mode also initializes genesis/accounts/applied progress in
that same write; see [committed application](raft-application.md). Opening with a
different mode fails instead of silently upgrading an existing database.

`meta/identity` contains the following concatenation:

| Field | Bytes |
| --- | --- |
| ASCII `RXM1` (legacy numeric) or `RXM2` (named membership) | 4 |
| Cluster identifier length | 2 |
| Cluster identifier, ASCII letters/digits/`._-` | 1–128 |
| Local numeric node ID | 4 |
| Member count (odd, 3–31) | 1 |
| Sorted unique nonzero member IDs | 4 each |
| `RXM2` only: each member name's length then ASCII bytes, in member order | 1 + 1–128 each |
| Genesis digest | 32 |
| Keccak-256 checksum of the preceding bytes | 32 |

The local ID must belong to the member set. The genesis digest is Keccak-256 of the
exact `encode_genesis` record, and cannot be zero. In `RXM2`, names are strictly
sorted, unique identifiers using the same character rules as cluster names, and
the numeric IDs must be exactly 1 through the member count. Each name is thus
permanently bound to its one-based position. The maximum named record is 4,326 bytes.

`make_named_manifest` constructs this mapping without silently sorting names.
`configuration_digest` produces the shared peer commitment described in
[Raft wire](raft-wire.md); unlike the storage manifest hash, it excludes local
node identity. The codec accepts this manifest directly and exposes the same
manifest for store/Core initialization. Raft state records bind the full encoded
manifest, including names, so changing a name invalidates recovery of its saved
votes/log. A runtime must still verify its endpoint routing against these names.

An empty name list explicitly selects the original `RXM1` format; its existing
encoding is preserved for development stores. The wire codec rejects such
numeric-only identities. Supplying names to an existing numeric-only store, or
removing/renaming names on a named store, fails identity comparison before ledger
replay. There is no automatic migration or fallback. A future migration requires
an explicit offline protocol that preserves vote/log identity.

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

Fifteen offline record tests cover round trips, frozen endian offsets, mutations,
every truncated prefix, identity binding, valid-checksum malformed records, and
legal/illegal state transitions. Five-node simulations now encode/decode every
stored update and apply the same transition guard, including crash/restart and
partition histories. These still use memory for storage.

Named-manifest tests additionally cover maximum-size records, truncation, malformed
names with recomputed checksums, positional numeric IDs, format separation,
configuration preimage stability, and saved-vote rejection after a member rename.
The serialized five-node simulations recover their codec from the named manifest
and bind every stored state image to that exact identity.

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
The ledger-enabled store now includes accounts, receipts, applied progress, startup
audit/replay, and separate database/application-kill tests as described in
[committed application](raft-application.md). Next verify real storage and connect
the gRPC driver before claiming durable replicated transfers.
Two additional ledger database tests check named identity retention through replay,
rename/downgrade rejection, and refusal to silently upgrade a numeric-only store.
Their source passes syntax checking; actual RocksDB execution remains pending.
