#include "rollupx/transaction_wire.hpp"
#include "../ledger_fixture.hpp"
#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace rollupx;
using namespace rollupx::test;
TEST(TransactionWire, RoundTripsRealGeneratedProtobufAndSignedEnvelope) {
  const auto transaction = ledger_transaction();
  const auto message = transaction_to_proto(transaction);
  EXPECT_EQ(transaction_from_proto(message), transaction);
  EXPECT_EQ(message.amount(), 10U); EXPECT_EQ(message.nonce(), 0U); EXPECT_EQ(message.chain_id(), 31337U);
  EXPECT_EQ(parse_transaction_wire(serialize_transaction_wire(transaction)), transaction);
  EXPECT_LE(message.ByteSizeLong(), kMaxTransactionWireBytes);
}
TEST(TransactionWire, RejectsEachRedundantFieldMismatch) {
  const auto valid = transaction_to_proto(ledger_transaction(1));
  auto message = valid; message.mutable_sender()->at(0) ^= 1;
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.mutable_recipient()->at(0) ^= 1;
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.set_amount(valid.amount()+1);
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.set_nonce(valid.nonce()+1);
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.set_chain_id(valid.chain_id()+1);
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
}
TEST(TransactionWire, RejectsEveryIncorrectBinaryFieldLength) {
  const auto valid = transaction_to_proto(ledger_transaction());
  for (const auto* name : {"id", "sender", "recipient", "signature", "canonical_payload"}) {
    const auto* field = valid.GetDescriptor()->FindFieldByName(name);
    ASSERT_NE(field, nullptr);
    const auto original = valid.GetReflection()->GetString(valid, field);
    for (const auto size : {std::size_t{0}, original.size()-1, original.size()+1}) {
      auto message = valid;
      message.GetReflection()->SetString(&message, field, std::string(size, '\0'));
      EXPECT_THROW(transaction_from_proto(message), std::invalid_argument) << name << " size=" << size;
    }
  }
}
TEST(TransactionWire, RejectsModifiedPayloadIdAndSignature) {
  const auto transaction = ledger_transaction(); const auto valid = transaction_to_proto(transaction);
  auto message = valid; message.mutable_id()->at(0) ^= 1;
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.mutable_canonical_payload()->at(0) ^= 1;
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  message = valid; message.mutable_signature()->at(64) = 3;
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  auto invalid = transaction; invalid.id[0] ^= 1;
  EXPECT_THROW(transaction_to_proto(invalid), std::invalid_argument);
  EXPECT_THROW(serialize_transaction_wire(invalid), std::invalid_argument);
}
TEST(TransactionWire, EnforcesRawAndParsedMessageBoundsAndKnownSchema) {
  const auto transaction = ledger_transaction(); auto message = transaction_to_proto(transaction);
  message.GetReflection()->MutableUnknownFields(&message)->AddVarint(99, 1);
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  EXPECT_THROW(parse_transaction_wire(message.SerializeAsString()), std::invalid_argument);
  message = transaction_to_proto(transaction); message.set_signature(std::string(513, 'x'));
  EXPECT_THROW(transaction_from_proto(message), std::invalid_argument);
  EXPECT_THROW(parse_transaction_wire(std::string(513, '\0')), std::invalid_argument);
  EXPECT_THROW(parse_transaction_wire(""), std::invalid_argument);
  EXPECT_THROW(transaction_from_proto(v1::Transaction{}), std::invalid_argument);
}
TEST(TransactionWire, RejectsTruncationInvalidTagsAndMalformedVarints) {
  const auto bytes = serialize_transaction_wire(ledger_transaction());
  for (std::size_t size = 0; size < bytes.size(); ++size)
    EXPECT_THROW(parse_transaction_wire(std::string_view(bytes).substr(0, size)), std::invalid_argument) << size;
  EXPECT_THROW(parse_transaction_wire(bytes + '\0'), std::invalid_argument);
  EXPECT_THROW(parse_transaction_wire(bytes + std::string("\x20\x80", 2)), std::invalid_argument);
}
TEST(TransactionWire, WireFieldOrderIsNotTheSigningPreimage) {
  const auto transaction = ledger_transaction(); const auto original = transaction_to_proto(transaction);
  std::string reordered;
  // Serialize each field separately, in reverse order, with an equal duplicate.
  // Protobuf permits this; authentication uses the canonical payload only.
  for (int i = original.GetDescriptor()->field_count()-1; i >= 0; --i) {
    auto part = original;
    for (int j = 0; j < original.GetDescriptor()->field_count(); ++j)
      if (i != j) part.GetReflection()->ClearField(&part, original.GetDescriptor()->field(j));
    reordered += part.SerializeAsString();
  }
  v1::Transaction duplicate; duplicate.set_amount(original.amount());
  reordered += duplicate.SerializeAsString();
  EXPECT_EQ(parse_transaction_wire(reordered), transaction);
  duplicate.set_amount(original.amount()+1); reordered += duplicate.SerializeAsString();
  EXPECT_THROW(parse_transaction_wire(reordered), std::invalid_argument);
}
TEST(TransactionWire, IntegerBoundsAndSemanticRejectionRemainExecutionResponsibilities) {
  const auto transaction = ledger_transaction(std::numeric_limits<std::uint64_t>::max(),
    std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(parse_transaction_wire(serialize_transaction_wire(transaction)), transaction);
  const auto zero = ledger_transaction(0, 0);
  EXPECT_EQ(transaction_from_proto(transaction_to_proto(zero)), zero);
  const auto genesis = ledger_genesis();
  EXPECT_EQ(execute_transfer(genesis.accounts, genesis.chain_id, authenticate(zero)).result, TransferResult::zero_amount);
}
TEST(TransactionWire, QueryKeysRequireExactLengthsAndKnownFields) {
  v1::TransactionId id; v1::AccountAddress account;
  EXPECT_THROW(transaction_id_from_proto(id), std::invalid_argument);
  EXPECT_THROW(account_address_from_proto(account), std::invalid_argument);
  const auto transaction = ledger_transaction(); const auto message = transaction_to_proto(transaction);
  id.set_id(message.id()); account.set_address(message.sender());
  EXPECT_EQ(transaction_id_from_proto(id), transaction.id);
  EXPECT_EQ(account_address_from_proto(account), authenticate(transaction).sender);
  id.mutable_id()->push_back('x'); EXPECT_THROW(transaction_id_from_proto(id), std::invalid_argument);
  account.mutable_address()->pop_back(); EXPECT_THROW(account_address_from_proto(account), std::invalid_argument);
  id.set_id(message.id()); id.GetReflection()->MutableUnknownFields(&id)->AddVarint(99, 0);
  EXPECT_THROW(transaction_id_from_proto(id), std::invalid_argument);
  account.set_address(message.sender()); account.GetReflection()->MutableUnknownFields(&account)->AddVarint(99, 0);
  EXPECT_THROW(account_address_from_proto(account), std::invalid_argument);
}
} // namespace
