# Offline transaction core — Phase 2 progress

The transaction core is independently buildable with OpenSSL 3 and GoogleTest.
It does not require gRPC, RocksDB, Docker, or sockets. This permits meaningful
verification while the Phase 1 full-network acceptance gate remains blocked.
It is not a durable ledger, consensus implementation, or running rollup.

## Build and run

```sh
cmake -S . -B build-core -DROLLUPX_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel 2
ctest --test-dir build-core --output-on-failure
```

Use a separate directory and `-DROLLUPX_SANITIZE=ON` for ASan/UBSan. For the cached
tools on the initial host, use the CMake executable/GTest_DIR documented in
IMPLEMENTATION_PLAN.md and `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`.
The explicit core-only mode registers only offline tests. The full default build
still requires and tests networking/storage dependencies.

```sh
./build-core/rollupx-wallet keygen /tmp/alice.rollupx-key
./build-core/rollupx-wallet keygen /tmp/bob.rollupx-key
bob=$(./build-core/rollupx-wallet address /tmp/bob.rollupx-key)
signed=$(./build-core/rollupx-wallet sign /tmp/alice.rollupx-key 31337 "$bob" 42 0)
./build-core/rollupx-wallet verify "$signed"
```

These commands create unencrypted raw 32-byte private key files with mode 0600.
Private keys are never command-line arguments or printed by the wallet. Creation
is exclusive, so rerunning keygen at the same path fails. Reads reject symlinks,
nonregular files, wrong lengths, foreign owners, and group/world-readable files.
Keep keys outside the repository. Verification authenticates bytes and signatures;
it does not check a live account balance/nonce or submit a transaction.

## Cryptography

`crypto.hpp` exposes Keccak-256, secp256k1 key generation, Ethereum address derivation,
raw-digest signing, signature verification, and public-key recovery. OpenSSL's
private DRBG generates private keys and provider-based ECDSA signing nonces.
`EVP_PKEY_sign` signs the already-hashed payload without an extra hash or Ethereum
personal-sign prefix. The implementation uses OpenSSL 3 provider key imports,
avoids deprecated EC_KEY APIs, and owns resources with RAII.

Signatures enforce r in [1,n), s in [1,n/2], and recovery ID 0 or 1. Signing normalizes
s and identifies the matching recovery bit; the exceptionally rare recovery IDs
2/3 cause a fresh signing attempt. Recovery reconstructs R from (r, parity), checks
its order, and calculates Q = r^-1(sR - zG). The independent EVP verifier checks
r/s and the recovery bit must identify the same address. No shared mutable
cryptographic context exists; verification is tested concurrently.

PrivateKey is move-only and clears its owned bytes on destruction/move. Explicitly
exported byte copies remain the caller's responsibility. This is a prototype, not
an audited signing library or hardware-backed wallet.

Keccak uses a portable Keccak-f[1600] implementation with a 136-byte rate and the
Ethereum 0x01 suffix. SHA3-256 has a different suffix and is not substituted.
Mandatory known-answer tests include empty input, abc, EIP-155 data, and frozen
135/136/137/272-byte boundary vectors. On OpenSSL providers with KECCAK-256,
additional comparisons cover multiple input sizes up to 8192 bytes. Providers
without that digest explicitly skip only the optional oracle comparison; all
frozen vectors remain required.

Compatibility references: [EIP-155 example](https://eips.ethereum.org/EIPS/eip-155),
[EIP-2 low-s rule](https://eips.ethereum.org/EIPS/eip-2), and
[OpenSSL raw signing](https://docs.openssl.org/3.0/man3/EVP_PKEY_sign/).
RollupX uses its own documented payload, not Ethereum RLP transactions or EIP-712.

## Canonical encoding

The 77-byte payload in transaction-model.md is implemented and has a frozen
test vector in `tests/vectors/transaction-v1.json`. The fixture uses a public test
scalar and a fixed test-only ECDSA nonce, independently computed with integer
arithmetic. Real signing always uses OpenSSL's randomized signing implementation.

An offline envelope is exactly `id[32] || payload[77] || signature[65]` (174 bytes).
Decoding requires exact length, domain, payload hash/ID agreement, and a valid
signature for the payload's sender. Tests flip every byte of a valid envelope and
require rejection. The future protobuf adapter must also compare redundant fields;
it has not yet been connected to the disabled cluster transaction RPCs.

## Deterministic reference execution

`execute_transfer` is a pure sequential transition on a fixed 16-slot account
registry. Validate state first with `validate_accounts`. It rejects wrong chains,
zero amount, self-transfers, unknown accounts, wrong nonce, insufficient balance,
nonce exhaustion, and recipient overflow, without changing any account. Successful
transfers increment only the sender nonce and conserve total supply. Restore/genesis
validation rejects duplicate addresses, nonempty unused slots and supply overflow;
genesis additionally requires zero account nonces.

`ExecutionState` authenticates each input, caches its result by payload ID, and
returns an existing result on retries. A semantically rejected ID retains its
original rejection even if a later state would permit it. Malformed signatures/IDs
are admission exceptions and do not consume receipts. Receipt capacity is bounded;
when exhausted, new IDs fail without mutation while existing retries still work.
Receipt insertion precedes publishing account changes for exception safety.

This class is single-owner, in-memory, and intentionally not used by the node's
public API. Its `applied` result is a reference execution outcome, **not durable
confirmation**. It must not be used as a substitute for the planned synchronous
RocksDB WriteBatch covering accounts, receipts, batch data, and applied index.

## Remaining Phase 2 work

The RocksDB adapter, durable-query CLI, and explicit standalone development mode
now exist as source; real-database builds and crash/replay tests remain blocked by
missing RocksDB. See [persistence.md](persistence.md). Next verify that path,
validate redundant protobuf fields, and integrate queries without enabling
nonconsensus cluster writes. Do not enable SubmitTransaction in the five-node
deployment until the replicated application path exists. Full Phase 1 validation
and Phase 2 durability remain acceptance gates before Raft integration.
