# Version 1 API contracts

`transaction.proto` defines canonical transaction transport, IDs, receipts, and
binary Merkle proof envelopes. See ../docs/transaction-model.md for byte lengths,
domain separation, integer semantics, and proof shape. Protobuf is an RPC envelope,
not a signature encoding. Schema declarations do not imply implementation.

`ledger.proto` defines the public `rollupx.v1.Ledger` service. With `--genesis` and
`--public-peers`, the replicated node implements:

| Method | Current behavior |
| --- | --- |
| SubmitTransaction | Authenticate, admit on leader, wait for durable applied receipt; retries return the original receipt |
| GetTransaction | Local durable ID-based receipt lookup; NOT_FOUND is not proof of global absence |
| GetBalance | Atomic local applied balance and observation index |
| GetAccountNonce | Atomic local applied nonce and observation index |
| GetBlock / GetBatch | Applied transfer batch at a Raft index; roots/commitments empty; no-op index returns NOT_FOUND |
| GetClusterStatus | REPLICATED_LEDGER, role, term, leader, progress, peer observations and leader readiness |
| GetLeader | Known leader public endpoint and term; advisory, may become stale |
| GetTransactionProof | UNIMPLEMENTED |

Reads are not linearizable; followers may lag. Each balance/nonce response carries
its snapshot's applied index. A successful submission returns APPLIED or a
previously committed REJECTED result, never a speculative PENDING acknowledgment.
Malformed input returns INVALID_ARGUMENT; state admission rejection returns
FAILED_PRECONDITION without a durable receipt; bounded capacity returns
RESOURCE_EXHAUSTED. Leadership loss, cancellation and deadlines can be ambiguous
after append. See [replicated node](../docs/replicated-node.md) for the full contract.

`ClusterStatus.peers` excludes the queried node. Reachability means a successful
outbound Probe within `peer_stale_after`; it is not a quorum acknowledgment. Probe
counters restart at process restart. `transport_ready` requires the entire mesh;
`ledger_ready` requires a healthy leader with its current-term barrier applied.
A majority can commit even when the full transport mesh is unavailable.

Without the ledger options, the node retains NETWORKING_ONLY diagnostics:
GetClusterStatus/probes work, GetLeader returns UNAVAILABLE and ledger methods
and internal Raft RPCs return UNIMPLEMENTED.

`consensus.proto` defines the separate internal listener:

* `PeerTransport.Probe`: validates version (1), cluster, canonical topology,
  and another member's claimed sender ID. Wrong version/cluster/topology returns
  FAILED_PRECONDITION; unknown/self sender returns PERMISSION_DENIED.
* `Raft.RequestVote` and `Raft.AppendEntries`: implemented codec and gRPC adapters
  with configuration/route/request correlation. The owner persists state and
  applies committed entries before a successful acknowledgment.
* `Raft.InstallSnapshot`: planned schema, UNIMPLEMENTED.

Only the public service is registered on the public listener. Unknown methods or
services return UNIMPLEMENTED. Servers cap sent/received messages at 1 MiB and apply
a gRPC resource quota. Ingress, submission and per-peer dispatch are bounded.
Transport is private-development plaintext; configuration IDs are not cryptographic
peer authentication. No RPC response guarantees future availability.
