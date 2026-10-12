# Single-owner Raft runtime

`rollupx_runtime` owns a real worker thread, a bounded inbox, and one Raft core.
Concurrent callers use copyable weak handles to enqueue validated protocol events
or proposals. Persistence, committed application, protocol decisions, and outbound
dispatch all run on the owner. The gRPC driver is still pending; the existing node
does not instantiate this runtime or accept replicated transfers yet.

## Ownership and callbacks

Construction validates Core/configuration, starts the owner, and waits for startup
application to reach the recovered commit index. An application failure throws from
construction after joining the worker. No handle is exposed before startup succeeds.

The following integration is implemented at the callback boundary and has a real
RocksDB test awaiting execution:

```cpp
// Identity/configuration must agree with the same named wire manifest.
raft::Store store(directory, manifest, genesis);
raft::Config config{manifest.node_id, manifest.members};
raft::Runtime runtime(config, store.load(),
  [&store](const raft::State& state) { store.persist(state); },
  [&store](raft::Index) { return store.apply_committed(); },
  [&transport](const raft::Envelope& message) {
    return transport.try_enqueue(message); // Copy into a bounded transport queue.
  });
```

`transport.try_enqueue` above is the required driver interface, not an existing
transport implementation. The store and transport must outlive the runtime. A real
driver must independently seed election randomness and verify its named endpoint
mapping against the manifest. Runtime callbacks may request shutdown or enqueue
work through handles; they must not wait on that work, join their own runtime, or
destroy the runtime. A same-owner call to `stop()` throws instead of deadlocking.

The runtime calls application at startup, including commit zero, and whenever commit
advances. The callback must return exactly that durable applied index. A callback
returning success without durable work violates this contract; the runtime cannot
prove an arbitrary callback's storage behavior. No-op/transfer ordering and receipt
semantics remain those of the [application bridge](raft-application.md).

## Admission, memory, and results

Default limits are 256 pending events and 8 MiB of charged event/payload storage.
Both include the currently executing event, so dequeue does not free its budget
before completion. Limits can be configured within 1–4,096 events and at most
64 MiB. Charges include event storage, vector capacity, and retained string capacity;
a small string carrying a large reserved allocation cannot bypass admission.
Fixed-size queue/future/allocator overhead is bounded by event count but is not an
exact RSS budget. The Raft log, per-peer flights, transport buffers, and caller-owned
completed futures have their own bounds and are outside this inbox accounting.

Each submission returns a `Ticket`:

| Admission | Meaning |
| --- | --- |
| `queued` | Future is valid; event is retained within the budget |
| `full` | Current event/byte capacity is exhausted; no event was queued |
| `invalid` | Empty/oversized proposal, oversized append payload/container, or event larger than the whole budget |
| `stopped` | Owner stopped/failed, or the weak handle has expired |

Use the [wire codec](raft-wire.md) to validate untrusted messages before submitting
them. Runtime admission bounds memory; it is not full protobuf/identity/transaction
authentication. An invalid typed protocol event that reaches Core fails the runtime
closed. Client transfer authentication and sequencing admission remain a driver task.

Every accepted ticket resolves to an `EventResult`. `processed` means the event's
protocol/application work completed; an incoming unary request may have a reply.
Replies are exposed only after required persistence and committed application.
`proposed_index` reports a durable local append, **not transaction confirmation**:
the entry may still be uncommitted and replaced later. Confirmation requires the
durable transaction receipt. A follower/no-quorum/uncommitted-barrier proposal is
`rejected` without stopping the node. Other unexpected protocol/storage/application
errors produce `failed`, stop admission, and preserve the original exception through
`Runtime::failure()`.

## Scheduling and outbound work

Automatic ticking uses a steady-clock wait, by default every 10 ms. Due ticks run
even while the inbox is busy; each loop can also execute one queued event so slow
ticks do not indefinitely starve pending traffic. An injected monotonic clock and
explicit `handle.tick()` support deterministic tests with automatic ticks disabled.
A backward clock stops the runtime instead of altering timer semantics.

The nonblocking dispatch callback receives outbound requests only. A `false` return
means transport saturation/loss and increments `outbound_dropped`; it is never
converted into a Raft acknowledgment. Raft's normal heartbeat/election retries
handle loss. A thrown dispatch error stops the runtime. Unary responses stay in
the event result so a future gRPC handler can encode them against the original call.
Transport threads must retain request correlation, enforce deadlines and their own
queue limits, and submit valid responses through a handle. They never access Core.

`RuntimeStatus` is a mutex-protected snapshot: role, term, leader, commit/applied
indices, election count, pending events/bytes, admission rejections, outbound drops,
and stopped/failed flags. It is diagnostic state, not a linearizable account read.
After a storage error, durable state may be ahead of this snapshot; reopen/audit
storage before creating a replacement runtime.

## Cancellation and shutdown

`Ticket::cancel()` prevents execution if the owner has not begun that event. It
does not remove the event's memory charge immediately. Once execution begins,
cancellation cannot roll back a durable write; the operation finishes normally and
the client must query/retry idempotently after an ambiguous timeout.

`request_stop()` closes admission and wakes the owner. An active operation finishes;
queued events resolve `stopped` without execution. `stop()` additionally joins the
worker and is safe to call repeatedly or concurrently from non-owner threads. No
callback runs after it returns. It must wait for active storage/dispatch callbacks
to return; there is no unsafe force-cancellation of an in-progress storage write.
Destruction performs the same stop/join. Callback handles contain only weak inbox
references: late callbacks reject work after destruction and cannot target a new
runtime instance, even if numeric RPC sequences restart.

## Verification

The runtime tests use real owner/producer threads and explicit futures, condition
variables, and deadlines. They cover single ownership; persistence/application
before reply; count/byte backpressure; reserved-capacity accounting; queued
cancellation; active/queued shutdown; concurrent stop; stale handles; ambiguous
storage failure; application failure; startup progress; clock regression; ordinary
leader rejection; outbound backpressure; owner self-join rejection; automatic
elections; shared signed-transfer replay; and 320 concurrent producer operations.
The ordinary tests use memory persistence, not RocksDB durability claims.

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure

cmake -S . -B build-tsan -DROLLUPX_CORE_ONLY=ON -DROLLUPX_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan --target runtime_tests --parallel 2
ctest --test-dir build-tsan -R '^runtime_tests$' --output-on-failure
```

Normal, ASan/UBSan, and ThreadSanitizer runtime tests pass on the development host.
The dedicated CI race-test job is configured; remote execution has not been
observed. The RocksDB callback integration/reopen test remains unexecuted because
RocksDB is missing. Bounded gRPC client/server integration, public durable queries,
and five-process replication/fault tests are the next acceptance gates.
