# Raft Protobuf boundary

## Implemented scope

`rollupx_raft_wire` maps the tested Raft core's vote/append requests and responses
to real generated `consensus.proto` messages. It provides bounded raw parsing,
field validation, and unary-call response correlation. It has no socket, thread,
storage, or consensus side effects. The node's Raft RPC handlers remain
UNIMPLEMENTED until real storage and runtime integration are verified.

Build and run the codec and serialized five-node simulations without gRPC/RocksDB:

```sh
cmake -S . -B build-wire -DROLLUPX_WIRE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wire --parallel 2
ctest --test-dir build-wire --output-on-failure
```

This tier requires C++20, OpenSSL 3, Protobuf/protoc, and GoogleTest. Repeat in a
separate build directory with `-DROLLUPX_SANITIZE=ON` for ASan/UBSan. Both the
protobuf CI job and full build include `raft_wire_tests`.

## Identity and configuration

Construct one immutable `WireCodec` per node with the cluster name, a strictly
sorted list of 3–31 unique member names (odd count), the genesis digest, and local
numeric ID. Numeric IDs are one-based positions in that list. The codec rejects
unsorted lists instead of silently changing their meaning. Cluster/member names
are 1–128 ASCII letters, digits, `.`, `_`, or `-`. Genesis cannot be the zero hash.

Every request and response includes a mandatory `RaftContext` with:

| Field | Validation |
| --- | --- |
| `protocol_version` | Exactly 1 |
| `configuration_id` | Exact 32-byte digest described below |
| `sender_id`, `recipient_id` | Configured members, distinct, correct local direction |
| `request_term` | Positive original request term, including on responses |
| `rpc_id` | Core's nonzero append sequence; zero for vote calls |

The configuration digest is Ethereum Keccak-256 of this concatenation:

1. ASCII `ROLLUPX_RAFT_WIRE_V1`, without terminator.
2. One-byte cluster-name length followed by its ASCII bytes.
3. One-byte member count; for each member in sorted order, one-byte name length
   followed by its ASCII bytes.
4. The 32-byte digest of the exact encoded ledger genesis.

Local identity is excluded so peers derive the same configuration digest. Endpoint
addresses are not consensus identities. The codec now uses the shared `RXM2`
[named storage manifest](raft-persistence.md) for this mapping. `WireCodec(Manifest)`
rejects numeric-only legacy identities. `codec.manifest()` exposes the exact
identity to supply to `raft::Store` and derive Core's local ID/member list. Reopening
the store with renamed members or a different format fails before state replay.
The future runtime must also bind each name to its configured endpoint and verify
Core's identity against the same manifest before processing any events.

The previous string/list constructor remains a convenience that constructs this
same named manifest. Digest calculation is shared with storage identity code;
the `ROLLUPX_RAFT_WIRE_V1` preimage remains unchanged. Named-store persistence is
implemented through the existing atomic manifest/state initialization, with real
database verification still pending.

The request's existing cluster and candidate/leader fields must agree with context.
Responses are checked against the locally retained original call: peer, recipient,
request term, request kind, and append sequence must all match. A response from a
newer term may reject an old request and cause the core to step down; it cannot
claim success/grant for that old request. Duplicate replies to the same call can
decode; the core remains responsible for ignoring already-completed/stale effects.

These checks detect inconsistent configuration and accidental misrouting. They
are not cryptographic peer authentication and do not provide Byzantine consensus.
Plaintext transport is suitable only for the documented private development cluster.

## Bounds and protocol validation

Raw vote requests and all responses are limited to 4,096 bytes. Append requests
are limited to 528,384 bytes: 512 KiB command data plus 4 KiB framing allowance.
Raw size is checked before invoking the Protobuf parser; already-decoded messages
are checked again. Unknown fields are rejected at the root, context, and each
log-entry level. Missing context is rejected, so old context-free schemas cannot
silently interoperate with this codec.

Vote log positions must have matching zero/nonzero index/term, be within the
4,096-entry log bound, and not claim a log term newer than the request term.
Append decoding checks:

* At most 16 entries and 512 KiB total command bytes, each command at most 174,048
  bytes, all positions within the bounded core log.
* Explicit entry indices are contiguous after the claimed previous position.
* Entry terms are positive, nondecreasing, and no newer than the request term.
* Previous position/term are consistent and commit progress is within the log bound.
* A successful reply matches the exact last sent index and has no conflict hint.
* A failed reply has no matched index and a valid conflict index. For equal terms,
  the conflict must be at or before the request's previous index.

`conflict_term` is currently unsupported and must be zero. `InstallSnapshot` has no
codec or runtime implementation yet. Transfer commands are opaque at this layer;
the application/store boundary separately authenticates every signed envelope.

Protobuf serialization is not a canonical signing format. The adapter accepts
Protobuf's legal encodings and validates the resulting values. Serialization
helpers only serialize messages; use `encode_request`/`encode_response` to validate
locally generated envelopes and `parse_*`/`decode_*` for received input.

## Runtime integration contract

The future transport driver must use the correct method decoder before placing
an event on the implemented [single-owner runtime](raft-runtime.md) queue. It must retain the original outbound
request in its callback; deriving correlation from an untrusted response defeats
the checks. Only a successful RPC can supply a response; timeout/cancellation is
transport failure, never an acknowledgment. Callbacks from a stopped runtime must
be discarded before constructing a replacement core.

On the receiver, send a response only after `Core::receive` returns successfully,
so its required persistence has completed. Encode that response against the decoded
originating request. Other requests generated by the same event start new calls.
Do not wait on network completion while owning the Core event loop. Apply committed
entries through the [application bridge](raft-application.md), and confirm clients
only after their durable receipts exist. The runtime now enforces persistence and
application before exposing a unary reply; gRPC scheduling and client confirmation
remain driver work.

## Verification and remaining gate

Unit tests cover configuration mismatch, absent/unknown fields, wrong routing,
impossible positions, noncontiguous entries, term ordering, size/count boundaries,
malformed raw buffers, unsupported methods, stale-call correlation, and higher-term
rejections. Invalid traffic is checked before Core delivery and cannot change the
test's persistent term or write count.

Two deterministic five-node scenarios serialize every request and response through
generated Protobuf code. They elect leaders, replicate signed transfers, apply
committed entries, kill/restart a simulated leader, retry a transfer, isolate a
leader with one follower, and heal divergent logs. Committed prefixes are compared
after each delivered event, and final balances/nonces/receipts are checked. Time
and message delivery are simulated; persistence is an encoded memory image.

Passing these tests demonstrates the serialization/protocol/application boundary,
not socket networking, fsync durability, real crash recovery, or measured failover
performance. Real RocksDB suites, gRPC service/client integration with the tested
runtime, and five-process fault tests remain the next gates.
