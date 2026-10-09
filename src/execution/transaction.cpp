#include "rollupx/transaction.hpp"
#include <algorithm>
#include <string_view>

namespace rollupx {
namespace {
constexpr std::string_view domain = "ROLLUPX_TX_V1";
static_assert(domain.size() + 8 + 20 + 20 + 8 + 8 == kPayloadBytes);
void write_integer(Payload& payload, std::size_t offset, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) payload[offset+i] = static_cast<Byte>(value >> (56-8*i));
}
std::uint64_t read_integer(std::span<const Byte> payload, std::size_t offset) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < 8; ++i) result = (result << 8) | payload[offset+i];
  return result;
}
} // namespace
Payload encode_payload(const Transfer& transfer) {
  Payload result{};
  std::copy(domain.begin(), domain.end(), result.begin());
  write_integer(result, 13, transfer.chain_id);
  std::copy(transfer.sender.begin(), transfer.sender.end(), result.begin()+21);
  std::copy(transfer.recipient.begin(), transfer.recipient.end(), result.begin()+41);
  write_integer(result, 61, transfer.amount); write_integer(result, 69, transfer.nonce);
  return result;
}
Transfer decode_payload(std::span<const Byte> payload) {
  if (payload.size() != kPayloadBytes || !std::equal(domain.begin(), domain.end(), payload.begin()))
    throw std::invalid_argument("incorrect transaction payload length or domain");
  Transfer result;
  result.chain_id = read_integer(payload, 13);
  std::copy_n(payload.begin()+21, 20, result.sender.begin());
  std::copy_n(payload.begin()+41, 20, result.recipient.begin());
  result.amount = read_integer(payload, 61); result.nonce = read_integer(payload, 69);
  return result;
}
SignedTransaction sign_transfer(const PrivateKey& key, const Transfer& transfer) {
  if (address(key) != transfer.sender) throw std::invalid_argument("signer does not match sender");
  SignedTransaction result; result.payload = encode_payload(transfer);
  result.id = keccak256(result.payload); result.signature = sign_digest(key, result.id); return result;
}
Transfer authenticate(const SignedTransaction& transaction) {
  const auto transfer = decode_payload(transaction.payload);
  if (keccak256(transaction.payload) != transaction.id) throw std::invalid_argument("transaction ID does not match payload");
  const auto signer = recover_address(transaction.id, transaction.signature);
  if (!signer || *signer != transfer.sender) throw std::invalid_argument("invalid transaction signature");
  return transfer;
}
std::array<Byte, kTransactionBytes> encode_transaction(const SignedTransaction& transaction) {
  std::array<Byte, kTransactionBytes> result{};
  std::copy(transaction.id.begin(), transaction.id.end(), result.begin());
  std::copy(transaction.payload.begin(), transaction.payload.end(), result.begin()+32);
  std::copy(transaction.signature.begin(), transaction.signature.end(), result.begin()+109);
  return result;
}
SignedTransaction decode_transaction(std::span<const Byte> bytes) {
  if (bytes.size() != kTransactionBytes) throw std::invalid_argument("incorrect transaction envelope length");
  SignedTransaction result;
  std::copy_n(bytes.begin(), 32, result.id.begin());
  std::copy_n(bytes.begin()+32, kPayloadBytes, result.payload.begin());
  std::copy_n(bytes.begin()+109, 65, result.signature.begin());
  (void)authenticate(result); return result;
}
} // namespace rollupx
