# Protobuf transaction validation

The `rollupx_wire` library converts between generated `rollupx.v1.Transaction`
messages and authenticated, fixed-size `SignedTransaction` envelopes. This path
has real protobuf tests and an offline CLI. It is not yet connected to a running
ledger RPC; the five-node service still returns UNIMPLEMENTED for transactions.

## Build and use

This build needs C++20, OpenSSL 3, GoogleTest, protoc, and the matching protobuf C++
runtime. It does not need RocksDB, gRPC, or listening sockets.

```sh
# Ubuntu 24.04 dependencies:
sudo apt-get install -y build-essential cmake ninja-build libssl-dev libgtest-dev libprotobuf-dev protobuf-compiler
cmake -S . -B build-wire -DROLLUPX_WIRE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wire --parallel 2
ctest --test-dir build-wire --output-on-failure
```

For ASan/UBSan, use another build directory and `-DROLLUPX_SANITIZE=ON`.
The installed protobuf runtime is not rebuilt with sanitizers. Reduced build modes
are mutually exclusive. The full build includes the same wire library and tests.

Given a signed envelope produced by `rollupx-wallet sign`:

```sh
protobuf_hex=$(./build-wire/rollupx-wire encode "$signed_envelope_hex")
./build-wire/rollupx-wire decode "$protobuf_hex"
```

`encode` emits protobuf message bytes as hex; `decode` emits the original 174-byte
signed envelope as hex. Both authenticate the input. Neither submits a request or
reports commitment. Errors exit nonzero and produce no successful output. No secret
key is needed for conversion. `scripts/wire-smoke.sh` tests the complete flow with
temporary wallet keys and checks rejection of a conflicting unsigned amount.

## Validation contract

* Raw transaction protobuf input is limited to 512 bytes before parsing. Empty,
  malformed, truncated, and over-limit input fails. Parsed-message conversion also
  bounds `ByteSizeLong()` and requires every binary field's exact length.
* ID is 32 bytes, sender/recipient 20, signature 65, canonical payload 77. Every
  redundant sender, recipient, amount, nonce, and chain ID must equal the decoded
  canonical payload. The ID must equal its Keccak hash and the low-s signature
  must recover the sender. No unsigned field is a source of execution authority.
* Unknown protobuf fields are rejected in version 1, including query-key messages.
  Adding semantics requires a coordinated protocol upgrade; silently dropping a
  new field would be unsafe.
* Protobuf field order is not canonical. Legal reordered fields and repeated
  scalar fields are interpreted by protobuf; their final values must match the
  signed payload. A conflicting final value fails. The signed payload, not a hash
  of protobuf bytes, defines transaction identity. Do not use protobuf serialization
  as a settlement commitment or assume it is unique across implementations.
* Query IDs/addresses require exact 32/20-byte values. Authentication does not check
  account existence, balance, expected chain, current nonce, or available quorum.
  Those checks belong to admission/ordered execution using ledger state.

`transaction_from_proto` validates an already parsed object; it cannot recover the
original wire length or detect discarded earlier duplicate values. A future gRPC
adapter must retain its transport size limit as well as this semantic validator.
The current server's overall receive limit remains 1 MiB.

## Verification and next integration gate

Nine GoogleTests cover conversions, every redundant field, all fixed lengths,
cryptographic corruption, unknown fields, size limits, every truncated prefix,
malformed tags/varints, alternate legal encoding, uint64 bounds, and query keys.
Tests use protoc-generated messages and the actual protobuf runtime, without a mock
parser. Local normal and sanitizer results are recorded in IMPLEMENTATION_PLAN.md.

Next, verify real RocksDB durability and the five-node gRPC transport. Integrate
these converters into public admission only when the replicated application path
exists; successful format validation must never be presented as durable execution.
