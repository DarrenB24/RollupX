# Version 1 API contracts

`transaction.proto` defines canonical transaction transport, IDs, receipts, and
binary Merkle proof envelopes. See ../docs/transaction-model.md for byte lengths,
domain separation, integer semantics, and proof shape. Protobuf is an RPC envelope,
not a signature encoding. The schema reserves future functionality; declarations
do not imply implemented execution or validation.

`ledger.proto` defines the public `rollupx.v1.Ledger` service:

| Method | Intended semantics | Phase 1 behavior |
| --- | --- | --- |
| SubmitTransaction | Leader submission; receipt state distinguishes pending/applied | UNIMPLEMENTED |
| GetTransaction | Durable ID-based receipt lookup | UNIMPLEMENTED |
| GetBalance | Local applied balance and observation index | UNIMPLEMENTED |
| GetAccountNonce | Local applied nonce and observation index | UNIMPLEMENTED |
| GetBlock / GetBatch | Full batch bytes and commitments by number | UNIMPLEMENTED |
| GetClusterStatus | Node identity, role/progress, peer observations | Implemented: NETWORKING_ONLY |
| GetLeader | Current leader endpoint and term, if known | UNAVAILABLE (no consensus) |
| GetTransactionProof | Ordered siblings/count/index against batch root | UNIMPLEMENTED |

`ClusterStatus.peers` excludes the queried node. Reachability means a successful
outbound Probe within `peer_stale_after`; it is not a quorum acknowledgment. Probe
counters restart at process restart. `transport_ready` requires every configured
peer to be reachable and the process operational. All Raft progress fields stay
unset/zero and role stays ROLE_UNSPECIFIED in this milestone.

`consensus.proto` defines the separate internal listener:

* `PeerTransport.Probe`: validates version (1), cluster, full canonical topology,
  and another member's claimed sender ID. Wrong version/cluster/topology returns
  FAILED_PRECONDITION, unknown/self sender returns PERMISSION_DENIED, startup or
  shutdown returns UNAVAILABLE. This is not authenticated transport.
* `Raft.RequestVote`, `Raft.AppendEntries`, and `Raft.InstallSnapshot`: planned
  schemas, all UNIMPLEMENTED. Persist-before-reply behavior is specified in
  ../docs/consensus.md and must be implemented/tested before enabling them.

Only the public service is registered on the public listener. Unknown methods or
services return UNIMPLEMENTED. Servers cap sent/received messages at 1 MiB and apply
a gRPC resource quota; outgoing probes have a deadline and one in-flight request
per peer. A local successful status response makes no guarantee of future consensus.

Future admission codes will distinguish malformed input, unauthenticated signature,
state preconditions, leader redirects, backpressure, and ambiguous timeout. These
must be frozen with execution tests; do not depend on unspecified future codes.
