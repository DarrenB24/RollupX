# Raft protocol and recovery plan

The isolated C++ Raft core and deterministic five-node simulations are implemented;
see [raft-core.md](raft-core.md) for tested behavior and limits. The deployed node
still has no Raft integration. Probe messages are connectivity checks, not
AppendEntries heartbeats or votes. The gRPC Raft methods remain UNIMPLEMENTED.
The versioned [Raft persistence format and store](raft-persistence.md) are now
implemented; actual RocksDB durability verification is still pending.

Membership is static and odd, default five; quorum is floor(N/2)+1. One mutex/event
loop owns protocol state. Timers use a monotonic injectable clock and seeded RNG
in deterministic tests. Initial heartbeat interval 50 ms, election timeout uniform
in [300, 600] ms; these are tunable and must be tested under storage/network delay.

On election timeout increment term, vote for self, and synchronously persist both
before sending RequestVote. Votes require a member candidate, current term, no
other vote in this term, and a log at least as up to date (last term then index).
Persist a granted vote before replying. On a greater term, persist term and cleared
vote before any response. Only valid leader traffic or a granted vote resets the
election timer. Election counts are local process metrics; term is durable.

AppendEntries checks previous index/term. A conflict can truncate only uncommitted
entries; conflicting committed entries are fatal corruption. Persist replacement
log bytes before replying success. Follower commit advances to
min(leader_commit, last index matched by this request), never decreases. Empty
heartbeats do not justify committing an unmatched divergent suffix.

The leader tracks matchIndex and nextIndex per peer. It advances commit to N only
if a majority has matchIndex >= N and log[N].term is its current term; this commits
earlier terms indirectly. Append a current-term no-op after election. Never count
RPC acceptance, unsynced writes, or another term's late reply as replication.
Commitment is durable before it is announced to the application worker.

Check-quorum demotes a leader without majority contact within its election bound.
The majority rule prevents minority commitment even before demotion. No lease is
used to claim linearizable reads. Phase 3 tests must cover a stale leader handling
client submissions and a former leader rejoining with conflicting uncommitted data.

## Recovery and snapshots

Open the DB exclusively, verify node/genesis identity and record formats, load
term/vote/log boundary, validate all index relationships, replay committed but
unapplied entries, then start elections and serving ledger reads. Fail closed on
corruption or missing committed log data. An application WriteBatch includes the
applied-index marker and all ledger effects, with WAL and sync enabled.

A snapshot is built from a consistent DB snapshot at an applied index and includes
accounts, receipts/deduplication IDs, batch/DA metadata, last included index/term,
and format/genesis identity. Bound chunk size and total bytes; verify hashes and
indices before installing a staged generation. Atomically publish a durable
generation pointer before truncating old log/state. Installation never moves term,
commit, or application progress backwards. Preserve a matching suffix where safe.

## Mandatory tests before Phase 3 completion

* Election, split vote, same-term vote reuse, higher-term step-down, stale RPC replies.
* No majority: no commit. Old-term entries need a current-term majority entry.
* Conflicting suffix repair; never truncate a committed index.
* Duplicate/reordered/delayed messages and independent receiver clocks.
* Crash before/after term, vote, append, commit marker, and application WriteBatch.
* Five real processes: kill leader, restart follower and leader, isolate one node,
  split 2/3, and interrupt a batch. Compare durable receipts and roots after healing.
* Snapshot install interrupted at every stage; restart and catch up from compaction.

Use an in-memory deterministic transport for exhaustive protocol scheduling and
real gRPC/real RocksDB subprocess tests for integration. Fake persistence is not
evidence of crash durability. All tests have explicit deadlines and observable
conditions rather than sleep-only assertions.
