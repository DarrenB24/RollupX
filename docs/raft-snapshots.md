# Storage-level snapshot recovery

The ledger-enabled `raft::Store` supports audited checkpoints and resumable,
atomic installation before attaching a Runtime. These are storage APIs, not a
live recovery protocol: `InstallSnapshot` still returns `UNIMPLEMENTED` and the
log remains bounded to 4096 entries / 8 MiB. Never publish directly behind a
running Core; it would retain an obsolete in-memory state.

## Capture and validation

`create_snapshot()` requires a nonempty, fully applied committed prefix. Under
the store lock it replays signed commands from genesis, verifies accounts and
every original receipt (including rejected transfers), and audits stored receipt
keys. Uncommitted entries and the source node's election term/vote are excluded.
The checkpoint is written synchronously to `snapshot/local`; `local_snapshot()`
validates its contents and correspondence to active committed history when read.

The `RXS1` envelope contains length-prefixed Raft and ledger records, a sorted
set of original receipts, and a Keccak checksum. Counts and lengths use big-endian
integers. The embedded Raft record binds the complete membership and genesis,
using the first member as the canonical node ID so another member can import it.
Existing nested record checksums remain intact. Decoding independently replays
the prefix and compares the full resulting ledger and receipt set. A valid
checksum alone cannot make forged account or receipt data acceptable.

The snapshot is limited to 16 MiB and each transfer chunk to 256 KiB. This first
format retains all signed committed history so receipts and batch queries remain
independently auditable. It provides no log compaction or increased log capacity.
Checksums detect corruption; they do not authenticate a sender or prove quorum
commitment. A future transport must enforce the Raft leader/term protocol.

## Staging and publication

1. Compute `snapshot_hash(image)` and send `SnapshotChunk{id, total_bytes,
   offset, data}` to `stage_snapshot()`. Each accepted change synchronously
   replaces a checksummed `RXSG1` record at `snapshot/staging`.
2. Resume using `staged_snapshot().next_offset` after reopening. Exact duplicate
   chunks are idempotent. Holes, changed bytes, overlapping extensions, changed
   totals and digest mismatches are rejected. Offset zero with a new ID can start
   a replacement generation. Completed bytes must pass full snapshot validation
   before the final chunk is stored.
3. Call `publish_staged_snapshot()` before creating the Runtime. It revalidates
   the image against the target's current state, rejects regressing progress or
   conflicting committed history, and requires the included term not to exceed
   the target's persisted term. Normal protocol handling must persist any newer
   leader term first. Installation never copies source election metadata.
4. One synchronous RocksDB WriteBatch publishes Raft state, ledger state,
   receipts and the local checkpoint, and deletes staging. The target's term and
   vote survive. A suffix with a matching boundary term is retained only after
   exact prefix comparison; a divergent uncommitted suffix is replaced.

An identical already-applied prefix returns `false` after publication; otherwise
the method returns `true` when active Raft/ledger state changes. Staging never
changes active accounts, receipts or progress. `discard_staged_snapshot()` removes
an unwanted generation. Startup does not automatically activate staging; a
corrupt staged record can be discarded after reopening without affecting active
state. Detected stored corruption or ambiguous write failure poisons the current
handle, requiring reopen. Invalid incoming chunks do not poison active state.

## Verification and remaining work

Codec tests cover portable identity, replay validation, forged records with
recomputed checksums, truncation, bounded chunks and exact retries. Real RocksDB
tests cover restart/resume, saved votes, suffix retention/replacement, conflicting
history, stale publication and failure recovery. Eight SIGKILL cases interrupt
checkpoint replacement, first/final chunk staging and publication immediately
before and after synchronous writes, then reopen twice and retry recovery.
They demonstrate process-crash behavior, not power-loss durability.

Next work is runtime-owned live snapshot transfer with term/correlation handling,
a compacted log representation and durable audit strategy, followed by controlled
network partition/delay and catch-up tests. This storage milestone does not enable
any of those behaviors implicitly.
