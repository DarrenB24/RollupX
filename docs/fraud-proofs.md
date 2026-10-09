# Bounded fraud-proof protocol (Phase 5 design)

No settlement contract is implemented yet. The first contract supports exactly
one transfer per L1 batch and the fixed 16-slot registry described in
transaction-model.md. L2 batching may be larger; publishing those batches requires
a later execution-trace protocol. They must not silently be treated as verified
by the bounded contract.

The contract stores chain ID, registry, genesis root, authorized publisher, challenge
duration, and ordered batch records. Only one unfinalized batch is permitted at a
time initially. submitBatch must extend the finalized root with batch number + 1,
one canonical payload/signature, its transaction commitment, its separate signature-
binding data commitment, and claimed post-root. Store the data commitment on-chain;
it is K(0x20 || canonical_payload_77 || signature_65), and is checked at submission.
Emit the full transaction bytes as L1 event data for this bounded protocol. Retain
them on L2 query endpoints too. An invalid previous root or malformed commitment
is rejected immediately. A batch is finalizable only after its challenge deadline
and if no proven fraud invalidated it. A proven invalid batch cannot become parent
to descendants. Replacement uses the same next number and starts a new window.

## Witness and verification

Challenge identifies batch number and transaction index (must be zero), and supplies:

1. The exact canonical transaction bytes/signature matching the stored data
   commitment. A proof of the transaction ID alone cannot authenticate a signature.
2. Sender index, address, pre-balance, pre-nonce, and four siblings against pre-root.
3. Recipient index, address, pre-balance, pre-nonce, and four siblings against pre-root.

Require distinct in-range registry indices and correct addresses. Authenticate
**both original leaves** against the recorded pre-root before any execution checks.
Malformed or unauthenticated witnesses revert; unsupported accusations never win.

Decode the transaction, verify low-s signature/recovered address, chain, positive
amount, exact nonce, balance, and overflow bounds. An authenticated invalid
transaction proves fraud if the batch claims it is a valid transfer. A valid
transaction produces two updated leaves. Compute the resulting root by merging
the two original proof paths, replacing both leaves at their shared ancestor;
shared subtree hashes must agree. Never apply a second old-root proof naively to
the intermediate updated root. The computed root must equal the claimed post-root;
inequality proves fraud. Matching roots cause the challenge to fail.

This authenticates state transition correctness; it does not prove availability,
censorship resistance, or security of deposits/withdrawals. Bonds, reward economics,
general traces, and L1 reorg-aware publication are later work.

## Foundry gates

Shared C++/Solidity vectors must agree on Keccak, signature recovery, endianness,
leaf encoding, all sender/recipient index pairs, and two-leaf path merging. Test
valid commitments, wrong previous roots, signatures, nonces, insufficient balance,
overflow, invalid proofs, altered payloads, rejected false challenges, successful
post-root challenges, challenge-window boundaries, finalization, and permanent
rejection of finalizing a challenged batch. Fuzz every index pair and tampered
witness field. Anvil publication tests come after these contract tests pass.
