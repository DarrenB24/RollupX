# Initial transaction model

Canonical encoding, cryptographic authentication, and the in-memory reference
transition are implemented and tested. Durable execution and network submission
remain pending; tree/settlement sections below are still designs. See
[crypto-and-execution.md](crypto-and-execution.md).

Version 1 has no fees, EVM, or arbitrary calldata. Integer values are uint64 base
units. Accounts are 20-byte Ethereum addresses. Account state is (balance, nonce).
Transactions to unknown accounts and self-transfers are rejected initially to keep
the supported settlement witness narrow and unambiguous. The genesis registry is
immutable, has at most 16 distinct nonzero addresses, and assigns each an index.
Unused slots have a zero address, zero balance, and zero nonce. The genesis total
supply must fit uint64. There is no mint RPC.

## Version 1 encoding (frozen offline test vector)

The signed payload is the following concatenation, with no lengths or padding:

| Field | Bytes |
| --- | --- |
| ASCII `ROLLUPX_TX_V1` | 13 |
| chain ID, unsigned big-endian | 8 |
| sender | 20 |
| recipient | 20 |
| amount, unsigned big-endian | 8 |
| sender nonce, unsigned big-endian | 8 |

Total payload: 77 bytes. Digest and transaction ID are Keccak-256(payload).
Signature is r[32] || s[32] || recovery_id[1], where recovery_id is 0 or 1,
1 <= r < secp256k1 order, and 1 <= s <= order/2. Signing retries the extremely
rare recovery encodings 2/3. Ethereum ecrecover uses v = recovery_id + 27.
No personal-sign prefix is added. Recovered uncompressed public key (excluding
04 prefix) hashes to an address using the last 20 bytes of Keccak-256.

OpenSSL supplies curve/signature operations; Ethereum-compatible Keccak and
recovery must have known-answer and independent compatibility tests. RPC protobuf
messages carry all fields plus payload and signature. Inconsistent redundant
fields, noncanonical lengths, and unknown versions are rejected. Protobuf wire
serialization is never the signing preimage.

The generated-protobuf adapter and offline conversion CLI are implemented and
tested; see [transaction-wire.md](transaction-wire.md) for input limits, unknown
field policy, alternate wire encodings, and build commands. Ledger RPC integration
remains pending.

A valid transfer requires amount > 0, matching configured chain, known different
accounts, exact current nonce, balance >= amount, nonce < UINT64_MAX, and recipient
balance <= UINT64_MAX - amount. The sender loses amount and increments nonce;
the recipient gains amount. All checks precede any mutation.

An existing applied ID returns its original receipt. A duplicate inside a batch
does not execute twice. Deterministic rejected log commands create a receipt with
an explicit error and unchanged accounts; the exact policy for such commands in
settlement batches must be tested before Phase 3 is accepted. Client admission
failure does not create a committed receipt.

## Trees

Transaction leaf = K(0x00 || tx_id). Internal node = K(0x01 || left || right).
Pad to the next power of two with K(0x02). Empty tree = K(0x02). A proof includes
leaf index, leaf count, and exactly ceil(log2(count)) ordered siblings; index must
be below count. Reject extra/missing siblings. Signature bytes are retained with
the payload in DA; only a canonical, verified signature may enter a valid batch.

Account leaf = K(0x10 || index_u8 || address_20 || balance_u64_be || nonce_u64_be).
There are exactly 16 slots, depth 4, including unused slots. Account parent =
K(0x11 || left || right). Index bits from low to high select sibling orientation.
The registry address at each index is immutable. A balance or nonce proof is bound
to the specific root, index, and address. General sparse-address trees are deferred.

Batch metadata binds version, chain ID, monotonically increasing batch number,
transaction count, transaction root, a separate data commitment (including signatures),
previous state root, and resulting state root. For the bounded single-transfer L1
protocol, data commitment = K(0x20 || canonical_payload_77 || signature_65).
The transaction root alone does not bind signature bytes. Multi-transfer data
commitment preimages will be defined in shared vectors before implementation.
