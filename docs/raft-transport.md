# Bounded Raft transport

The transport connects the [single-owner runtime](raft-runtime.md) to the
[identity-bound wire codec](raft-wire.md). Peer scheduling and ingress handling
are verified locally with real threads and generated Protobuf messages. The gRPC
client/service and socket tests pass on native builds. The optional
[replicated node](replicated-node.md) composes this transport with its durable
ledger and public transaction APIs.

## Ownership and startup

`PeerDispatcher` owns one worker for each remote member. `RaftIngress` submits
validated requests through a weak runtime handle and waits for its result.
Neither accesses Core or the database directly. The runtime owner remains the
only thread that persists consensus state and applies committed entries.

The node driver opens/audits its named store, validates its config and
endpoint mapping against that same manifest, then constructs components in this
order:

1. `GrpcRaftClient(manifest, remote_endpoints)` and `PeerDispatcher(codec, call)`.
2. `Runtime` with the store persistence/application callbacks and dispatcher enqueue.
3. Bind the dispatcher to `runtime.handle()` exactly once.
4. `RaftIngress(codec, runtime.handle())`, `GrpcRaftService`, and a bounded server.
5. Register the service and start listening before reporting readiness.

Before binding, dispatch returns false; such early traffic is dropped and retried
by Raft. A stopped dispatcher cannot be restarted or rebound to a replacement
runtime. The client and dispatcher must outlive runtime callbacks; the ingress
must outlive service handlers. Unexpected backend exceptions close the dispatcher,
retain `failure()`, and are rethrown on the next dispatch. The driver must monitor
both runtime and dispatcher failure status and withdraw readiness.

## Outbound limits and response handling

Each peer has at most one active RPC and one pending request. For five nodes that
means four workers, four active calls, and four pending requests per node. A slow
peer cannot consume another peer's slot. A full slot returns false immediately
and increments backpressure; it never produces a synthetic acknowledgment.

Commands and messages retain the codec's limits: at most sixteen entries and
512 KiB of command bytes per AppendEntries, with a 528,384-byte wire bound.
Each retained job includes the original bounded envelope and its Protobuf request.
Count and payload limits bound application storage, but are not an exact RSS limit
for the allocator, Protobuf, or gRPC internals.

The default call deadline is 100 ms, configurable from 1 ms through 10 seconds.
It starts when a worker takes a pending request. Queueing can additionally last
up to the active call's remaining lifetime when the backend honors its deadline.
`PeerCall` implementations must honor both deadline and stop token and must stop
using the request before returning. Arbitrary user callbacks cannot be forcibly
cancelled safely.

The worker validates each response against the original recipient, configuration,
term, RPC sequence, and request bounds. Local deadline checks before handoff discard
late successes. RPC failures, missing/malformed responses, and a full/stopped runtime
inbox are counted separately. Successful handoff means admission to the runtime
queue, not commitment or durable transaction confirmation.

## Inbound limits and cancellation

Ingress defaults to sixteen concurrent handlers, a 500 ms maximum wait, and a 5 ms
cancellation polling interval. Saturation returns resource exhaustion. Requests
with invalid identities/fields never enter the runtime. Queued work is cancelled
on deadline, caller cancellation, close, or unwinding an exception. Already-started
persistence/application completes normally: timeout is an ambiguous result, not a
rollback. The caller retries using the existing Raft request semantics.

A successful unary response is available only after the owner's required durable
persistence and committed application callbacks complete. A storage failure yields
an explicit error and no successful acknowledgment. Actual durability depends on
the real store callback, verified by native persistence and crash tests.

## gRPC adapter

`GrpcRaftClient` requires an endpoint for every remote member and rejects extras,
missing members, and malformed endpoints. Channels are plaintext and intended for
the private development cluster. Configuration digests identify topology; they
do not cryptographically authenticate a peer. Malicious nodes and public-network
deployment remain outside the guarantees.

The adapter creates a fresh client context per call, sets a deadline, and connects
the stop token to `TryCancel`. Server handlers use context deadlines and cancellation
to bound ingress waits. These APIs follow the official
[client context](https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_context.html)
and [server context](https://grpc.github.io/grpc/cpp/classgrpc_1_1_server_context.html)
contracts. `configure_raft_server` sets message limits, a 64 MiB gRPC resource quota,
a sixteen-thread quota, and one completion queue with one to four synchronous
pollers. The gRPC quota is not a whole-process memory ceiling.

`RequestVote` and `AppendEntries` are implemented. `InstallSnapshot` retains the
generated `UNIMPLEMENTED` result; there is no snapshot success placeholder.

Shutdown must close ingress, stop/cancel outbound calls, shut down and wait for
the gRPC server's active handlers, stop/join the runtime, and only then destroy
callbacks and storage. `PeerDispatcher::stop()` joins workers and supports concurrent
external callers. A worker must not join or destroy its own dispatcher. An ingress
object must never be destroyed while another thread is inside `respond()`.

## Tests and build tiers

The wire tier runs twelve new tests: peer isolation and bounded queues, response
correlation, failure/deadline handling, cancellation/shutdown, expired handles,
ingress saturation, persistence/application ordering, queued cancellation, storage
failure, and a threaded five-replica partition/healing scenario. The latter uses
twenty peer workers and five owners, signed transfers and retries, serialized
Protobuf links, and checks identical committed prefixes and final account state.
Persistence is memory-backed; this is not five-process or disk-crash evidence.

```sh
cmake -S . -B build-wire -DROLLUPX_WIRE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wire --parallel 2
ctest --test-dir build-wire --output-on-failure

cmake -S . -B build-tsan -DROLLUPX_WIRE_ONLY=ON -DROLLUPX_TSAN=ON
cmake --build build-tsan --target runtime_tests transport_tests --parallel 2
ctest --test-dir build-tsan -R '^(runtime|transport)_tests$' --output-on-failure
```

With gRPC and its protoc plugin installed, the new tier also builds real loopback
socket tests without requiring RocksDB:

```sh
cmake -S . -B build-transport -DROLLUPX_TRANSPORT_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-transport --parallel 2
ctest --test-dir build-transport --output-on-failure
```

Four socket tests cover actual vote/append RPCs, malformed identities, unsupported
snapshots, expired/cancelled/closed calls, and endpoint mapping. They require successful
socket binding and fail rather than skip when unavailable. CI is configured to run
this tier normally and with ASan/UBSan; both passed in the recorded main-branch CI.
Native node tests now exercise RocksDB, public transactions, and five-process
crash recovery. Snapshots and broader process partition/delay scenarios remain
acceptance gates before performance work.
