# Transaction admission and durable confirmation

`rollupx_submission` connects authenticated transfers to the Raft runtime and
receipt reader. It is tested with real threads and the shared application planner,
including five runtimes exchanging serialized Protobuf messages. The native
[replicated node](replicated-node.md) now exposes this path through gRPC with an
audited RocksDB store and verified five-process crash recovery.

## Conservative leader admission

The runtime accepts an optional `ProposalCheck`, called only by its owner before
a leader appends a client proposal. A returned reason is a nonfatal admission
rejection; any exception stops the runtime and preserves the exception. This
includes a callback throwing `ProposalRejected`: only Core's own explicit
leadership/quorum rejection is recoverable. Followers never run client policy on
replicated entries; committed execution remains deterministic on every replica.

`ledger_proposal_policy` requires readers from the same audited ledger used by
the runtime's application callback. It validates a single-transfer command and
requires applied progress to equal the Core commit marker. A mismatch or corrupt
receipt is a storage/integration failure, not a rejected transaction.

Admission checks signatures, transaction identity, chain ID, registry membership,
amount, nonce, balance, and overflow using the existing deterministic transition
function. This produces a temporary result for validation without publishing any
account changes. Existing receipts preserve the original result, including a
durable rejection. Raft still independently requires an elected leader, recent
quorum, and a committed current-term barrier before appending.

Initially, only one uncommitted command may exist before a new transfer is admitted.
A competing transfer receives `busy`; it cannot validate against a balance/nonce
already reserved by an outstanding proposal. A timeout does not release that log
slot: the proposal can still commit. Normal Raft commitment or log repair resolves
it. This deliberately conservative mechanism precedes the planned bounded mempool,
batching, and reservation model. It makes no throughput claim.

The current log's entry and byte limits return `log_full` backpressure before a
client append. Snapshotting/compaction is still needed for sustained operation;
this admission check does not implement compaction or remove Raft's log limits.

## Client-facing semantics

`LedgerSubmission::submit` authenticates every request before trusting its ID,
including cached retries. It checks for an existing receipt, queues a proposal,
and waits for a receipt produced by committed application. Neither inbox admission,
a local log index, nor majority acknowledgment alone is confirmation.

| Result | Meaning |
| --- | --- |
| `confirmed` | Contains the original applied, durable receipt; inspect its transfer result |
| `rejected` | Leader admission rejected the transfer; no durable receipt was created |
| `invalid` | Malformed/signature-invalid transfer or invalid runtime event |
| `busy` | Concurrent-call limit, runtime capacity, outstanding command, or log capacity |
| `unavailable` | Stopped runtime, follower/no quorum, leadership change, or closed service |
| `deadline` / `cancelled` | Caller stopped waiting; execution may already have started |
| `failed` | Runtime or callback failure; no successful result is returned |

An existing receipt can be returned by a follower. Missing receipts do not prove
that another replica has not committed a transaction. After an ambiguous result,
retry the same signed transaction ID against the current leader or query a durable
receipt. Do not generate a new transfer merely because a request timed out.

An admission rejection is not a `LedgerReceipt` and must not become a successful
gRPC `REJECTED` receipt response. A future service adapter should map admission
failure to an explicit RPC error. Conversely, a replicated transaction with an
actual durable rejection receipt returns `confirmed` plus that receipt's rejection
result. Confirmation refers to durable processing, not necessarily a value transfer.

## Bounds, failures, and lifetime

Defaults allow sixteen concurrent submissions, a two-second maximum wait, and
five-millisecond polling for receipt/cancellation progress. Admission happens before
signature verification, so accepted callers bound verification work. There is no
unbounded submission queue, background verifier pool, or pending-ID map here.
Receipt readers must be thread-safe and finish in bounded time; this layer cannot
interrupt a blocked storage callback or prove that an arbitrary callback is durable.

All exits cancel a queued runtime ticket. Cancellation cannot interrupt an already
started durable operation. Leadership changes return an ambiguous unavailable
result if no receipt is visible. `close()` wakes waiters and closes admission;
callers must finish all `submit` calls before destruction. Weak runtime handles
report stopped after destruction and cannot attach to a replacement runtime.
Unexpected reader/cancellation callback exceptions fail the submission service
closed and remain available through `failure()`. The node driver must monitor
both this status and runtime/transport failures when deciding readiness.

## Required integration

The same store must supply all four callbacks; no standalone-ledger writer may
modify the replicated store concurrently:

```cpp
auto read_receipt = [&store](const Hash& id) { return store.receipt(id); };
auto policy = ledger_proposal_policy(genesis,
  [&store] { return store.ledger_state(); }, read_receipt);
raft::Runtime runtime(config, store.load(),
  [&store](const raft::State& state) { store.persist(state); },
  [&store](raft::Index) { return store.apply_committed(); },
  [&transport](const raft::Envelope& message) { return transport.try_enqueue(message); },
  runtime_options, {}, std::move(policy));
transport.bind(runtime.handle());
LedgerSubmission submission(runtime.handle(), read_receipt);
```

The replicated node implements this integration contract. Store, transport, and
captured callbacks outlive the runtime and submissions. See the node guide for
public query semantics, lifecycle/readiness, and five-process crash testing.

## Verification

The core build includes twelve submission tests and twenty runtime tests. Coverage
includes rejection without mutation, concurrent duplicates/conflicts, delayed
application after majority commitment, timeout followed by commitment/retry,
cancellation of a queued proposal, leadership loss, follower retries, durable
rejection receipts, invalid-signature retries, bounded calls, close, expired handles,
storage errors, and owner-policy failures. The wire build adds a five-runtime test
with concurrent submission retries and replicated balance/receipt agreement.

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
```

For thread race checks, configure a separate wire-tier build with
`-DROLLUPX_WIRE_ONLY=ON -DROLLUPX_TSAN=ON`, build `runtime_tests submission_tests
transport_tests`, and run `ctest -R '^(runtime|submission|transport)_tests$'` in
that build directory. These tests provide no TPS or hardware latency benchmark.
