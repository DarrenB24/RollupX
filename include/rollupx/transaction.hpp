#pragma once
#include "rollupx/crypto.hpp"

namespace rollupx {
inline constexpr std::size_t kPayloadBytes = 77;
inline constexpr std::size_t kTransactionBytes = 174;
struct Transfer {
  std::uint64_t chain_id{};
  Address sender{}, recipient{};
  std::uint64_t amount{}, nonce{};
  bool operator==(const Transfer&) const = default;
};
using Payload = std::array<Byte, kPayloadBytes>;
struct SignedTransaction {
  Hash id{};
  Payload payload{};
  Signature signature{};
  bool operator==(const SignedTransaction&) const = default;
};
Payload encode_payload(const Transfer& transfer);
Transfer decode_payload(std::span<const Byte> payload);
SignedTransaction sign_transfer(const PrivateKey& key, const Transfer& transfer);
// Throws invalid_argument for inconsistent IDs, malformed payloads or bad signatures.
Transfer authenticate(const SignedTransaction& transaction);
std::array<Byte, kTransactionBytes> encode_transaction(const SignedTransaction& transaction);
SignedTransaction decode_transaction(std::span<const Byte> bytes);
} // namespace rollupx
