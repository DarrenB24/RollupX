# Deterministic Raft protocol core

## Status

`rollupx::raft::Core` implements protocol decisions in C++20 and is tested with
five simulated nodes. It is not connected to the gRPC node. A [RocksDB Raft store](raft-persistence.md)
now exists as source with record-format tests; its actual database tests remain blocked.
No public API claims replicated commitment yet. The standalone ledger's durability
tests and real transport tests remain acceptance gates for that integration.

The baseline algorithm is [Raft, extended paper, Figure 2 and Section 5](https://raft.github.io/raft.pdf).
This implementation adds bounded inputs, correlated replication replies, a
current-term proposal barrier, and check-quorum. Its supported fault model is
crashes and network faults among trusted members, not malicious consensus peers.

## Ownership and interface

One event-loop owner calls `tick(now)`, `receive(envelope, now)`, and
`propose(command, now)`. Time is an injected, monotonically increasing uint64 count
of milliseconds; the class never reads a clock or sleeps. A seeded generator
chooses election intervals using an explicit mapping so the same seed reproduces
the same schedule across standard-library implementations. A runtime adapter must
provide an independently seeded generator and monotonic process clock.

The class is noncopyable. Its state reference is only valid for synchronous
inspection by that same owner; it is not a concurrent snapshot API. Returned
envelopes contain member identities and a typed request/response. A transport
driver sends them only after the method returns successfully. Unexpected driver
exceptions must stop that node rather than silently drop required storage work.

`State` contains current term, optional vote, the complete ordered log, and commit
index. An entry's vector position plus one is its index. Empty commands are Raft
no-ops; external proposals must be nonempty. Other command bytes are opaque to
consensus. Admission and committed execution authenticate transfers separately.

The persistence callback must atomically and synchronously store the entire
supplied state before returning. The core changes its cached persistent state only
after callback success. A callback exception, including an ambiguous failure after
the storage effect, fails the core closed: subsequent events and proposals fail.
Allocation failures likewise stop it. Reconstruct from verified durable state after
closing the failed instance. There is no durable default or fallback callback.

Term/vote changes precede vote requests/replies. Local entries precede replication;
follower appends and commit progress precede success replies. Leader commitment
precedes outgoing commit announcements. Recovered state validates vote membership,
term ordering, index bounds, and capacity, but storage checksums and cluster/genesis
identity must be verified by the future persistent adapter.

## Protocol behavior

Membership is static, odd, and between 3 and 31 distinct nonzero numeric IDs. The
event loop must map configured names to these IDs consistently and persist that
mapping with its identity; remote callers cannot choose arbitrary identities.
Default intervals are 50 ms heartbeat and 300–600 ms elections. These are algorithm
configuration values, not measured failover promises.

Candidates persist their next term/self-vote and collect distinct current-term
votes. Voters compare last log term before index and never grant another candidate
a vote in the same term. Only granted votes and valid current-term leader traffic
reset the election timer. Higher terms clear the vote durably and step down.

A new leader appends a current-term no-op. Client proposals require that this
term's barrier is committed and that a majority has recent contact. Replication
tracks each follower's matched and next indices. Commitment advances only when a
majority matches an entry from the current term, thereby committing its prefix.

Each peer has at most one outstanding append request, with a local RPC sequence
and request term. Retransmission retains the original request. Only its matching
response updates progress. Contact freshness uses the original send time; a late
or duplicate reply cannot extend leadership indefinitely. A leader without recent
quorum steps down after its election bound. Before demotion, an isolated leader
may retain previously admitted entries but cannot commit them without a majority.

Followers verify the previous position, return conflict hints, and replace only
conflicting uncommitted suffixes. An empty heartbeat neither truncates an unmatched
suffix nor commits it. Follower commitment is capped at the last index matched by
that request and never moves backward. A conflicting committed entry or different
command at the same index/term is an invariant failure and stops the core.

## Bounds and present limitations

* At most 4,096 retained entries and 8 MiB of command data per node.
* At most 174,048 bytes per command, sufficient for the current maximum encoded
  development batch. At most 16 entries and 512 KiB per append request.
* One retained request per peer; no unbounded transport queue inside the core.
* No snapshots or compaction yet. Capacity exhaustion rejects further growth;
  existing data is never discarded to make room. Saturated logs can block leader
  barriers and availability until compaction is implemented.

The persistence callback receives full-state copies for an explicit first contract.
That is not an optimized storage path. Incremental WAL mutations and asynchronous
IO require their own durable completion protocol before changing this boundary.
There is no ReadIndex, lease read, membership change, pre-vote, or leader transfer.
One outstanding request favors bounded state over pipelined replication throughput.

Commit index is not application progress or a client confirmation. The future
application adapter must atomically advance its applied marker for both no-ops and
transfer batches, preserving durable receipts and at-most-once execution. The
standalone ledger format cannot simply be reused as a Raft store: it assumes every
locally committed entry is a transfer batch and has no term/vote protocol.

## Tests and integration sequence

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure -R raft_tests
```

The tests cover persistent vote reuse, log freshness, duplicate/stale responses,
conflict repair, no-op barriers, old-term commitment, check-quorum, malformed
traffic, storage failures before/after effects, and recovered-state validation.
Five-node scenarios serialize/deserialize every persistence effect through the
versioned record format and validate transitions. They cover two crashed followers, crashed/restarted leaders, split
votes, isolated leaders, a 2/3 partition, and suffix convergence after healing.
Twenty seeded schedules add dropped, duplicated, delayed/reordered traffic and
different node clock offsets. Every delivered event checks election uniqueness per
term and consistency of all committed prefixes, including crashed nodes' history.

A signed-transfer scenario sends real canonical envelopes through the simulated
log, changes leaders, retries a transfer, and independently executes each replica's
committed prefix using the reference engine. Balances/nonces agree and the retry
does not execute twice. This remains a memory-backed protocol/execution test.

The next integration steps are real RocksDB validation of the implemented Raft
store and persistence callback, gRPC request/reply adapters, bounded owner queues,
and durable ordered application with no-op markers. Then run five real processes
through the same fault scenarios. Snapshot installation/compaction follows with
its own interrupted-publication tests. Simulation time and test runtime are not
benchmark results, and memory recorders do not demonstrate fsync or WAL durability.
