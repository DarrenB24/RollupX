#include "rollupx/transaction_wire.hpp"
#include <algorithm>
#include <google/protobuf/unknown_field_set.h>

namespace rollupx {
namespace {
template<std::size_t N> std::array<Byte, N> fixed_bytes(const std::string& bytes) {
  if (bytes.size() != N) throw std::invalid_argument("incorrect protobuf field length");
  std::array<Byte, N> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}
template<std::size_t N> std::string raw(const std::array<Byte, N>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void supported(const google::protobuf::Message& message, std::size_t limit) {
  if (message.ByteSizeLong() > limit) throw std::invalid_argument("protobuf request exceeds size limit");
  if (!message.GetReflection()->GetUnknownFields(message).empty())
    throw std::invalid_argument("unsupported protobuf fields");
}
} // namespace
SignedTransaction transaction_from_proto(const v1::Transaction& message) {
  supported(message, kMaxTransactionWireBytes);
  SignedTransaction result{fixed_bytes<32>(message.id()), fixed_bytes<kPayloadBytes>(message.canonical_payload()),
    fixed_bytes<65>(message.signature())};
  const Transfer claimed{message.chain_id(), fixed_bytes<20>(message.sender()),
    fixed_bytes<20>(message.recipient()), message.amount(), message.nonce()};
  // Compare every redundant field; callers must never execute unsigned metadata.
  if (claimed != decode_payload(result.payload)) throw std::invalid_argument("protobuf fields disagree with signed payload");
  (void)authenticate(result);
  return result;
}
v1::Transaction transaction_to_proto(const SignedTransaction& transaction) {
  const auto transfer = authenticate(transaction);
  v1::Transaction result;
  result.set_id(raw(transaction.id)); result.set_sender(raw(transfer.sender));
  result.set_recipient(raw(transfer.recipient)); result.set_amount(transfer.amount);
  result.set_nonce(transfer.nonce); result.set_chain_id(transfer.chain_id);
  result.set_signature(raw(transaction.signature)); result.set_canonical_payload(raw(transaction.payload));
  return result;
}
SignedTransaction parse_transaction_wire(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaxTransactionWireBytes)
    throw std::invalid_argument("invalid protobuf transaction size");
  v1::Transaction message;
  if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())))
    throw std::invalid_argument("malformed protobuf transaction");
  return transaction_from_proto(message);
}
std::string serialize_transaction_wire(const SignedTransaction& transaction) {
  const auto message = transaction_to_proto(transaction);
  std::string bytes;
  if (!message.SerializeToString(&bytes)) throw std::runtime_error("protobuf serialization failed");
  return bytes;
}
Hash transaction_id_from_proto(const v1::TransactionId& message) {
  supported(message, 34); return fixed_bytes<32>(message.id());
}
Address account_address_from_proto(const v1::AccountAddress& message) {
  supported(message, 22); return fixed_bytes<20>(message.address());
}
} // namespace rollupx
