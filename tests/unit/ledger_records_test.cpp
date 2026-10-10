#include "rollupx/ledger_records.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace rollupx;
class Records : public ::testing::Test {
 protected:
  PrivateKey key = PrivateKey::from_bytes(fixed_hex<32>("0000000000000000000000000000000000000000000000000000000000000001"));
  Address recipient = fixed_hex<20>("3535353535353535353535353535353535353535");
  Genesis genesis{31337, {}};
  void SetUp() override { genesis.accounts[0] = {address(key), 100, 0}; genesis.accounts[1] = {recipient, 0, 0}; }
  SignedTransaction tx(std::uint64_t nonce = 0, std::uint64_t amount = 10) {
    return sign_transfer(key, Transfer{31337, address(key), recipient, amount, nonce});
  }
  static std::optional<LedgerReceipt> none(const Hash&) { return std::nullopt; }
};
TEST_F(Records, RoundTripsAllPersistentTypes) {
  EXPECT_EQ(decode_genesis(encode_genesis(genesis)), genesis);
  const LedgerState state{5, genesis.accounts}; EXPECT_EQ(decode_state(encode_state(state)), state);
  const auto transaction = tx();
  const LedgerReceipt receipt{transaction.id, 3, 999, TransferResult::insufficient_balance};
  EXPECT_EQ(decode_receipt(encode_receipt(receipt)), receipt);
  const LedgerBatch batch{8, {transaction, transaction}}; EXPECT_EQ(decode_batch(encode_batch(batch)), batch);
  for (const auto index : {std::uint64_t{0}, std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max()})
    EXPECT_EQ(decode_progress(encode_progress(index)), index);
}
TEST_F(Records, ParsesBoundedExplicitGenesisWithoutImplicitFunding) {
  const auto text = "chain 31337\naccount " + hex(genesis.accounts[0].address) + " 100\naccount " + hex(recipient) + " 0\n";
  EXPECT_EQ(parse_genesis_text(text), genesis);
  EXPECT_THROW(parse_genesis_text("chain 0"), std::invalid_argument);
  EXPECT_THROW(parse_genesis_text("chain 1"), std::invalid_argument);
  EXPECT_THROW(parse_genesis_text(text + "garbage"), std::invalid_argument);
  EXPECT_THROW(parse_genesis_text("chain 1 account " + hex(recipient) + " -1"), std::invalid_argument);
  EXPECT_THROW(parse_genesis_text("chain 1 account " + hex(recipient) + " 18446744073709551616"), std::invalid_argument);
  EXPECT_THROW(parse_genesis_text(std::string(4097, ' ')), std::invalid_argument);
}
TEST_F(Records, RejectsEveryMutatedByteAndWrongType) {
  const auto encoded = encode_genesis(genesis);
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    auto changed = encoded; changed[i] ^= 1;
    EXPECT_THROW(decode_genesis(changed), std::invalid_argument) << i;
  }
  EXPECT_THROW(decode_state(encoded), std::invalid_argument);
  EXPECT_THROW(decode_genesis(encoded.substr(1)), std::invalid_argument);
  EXPECT_THROW(decode_genesis(encoded+"x"), std::invalid_argument);
  EXPECT_THROW(decode_batch(std::string(200000, 'x')), std::invalid_argument);
}
TEST_F(Records, BoundsBatchSizeAndReceiptPosition) {
  EXPECT_THROW(encode_batch(LedgerBatch{1, {}}), std::invalid_argument);
  const auto transaction = tx();
  EXPECT_THROW(encode_batch(LedgerBatch{0, {transaction}}), std::invalid_argument);
  EXPECT_THROW(encode_batch(LedgerBatch{1, std::vector<SignedTransaction>(1001, transaction)}), std::invalid_argument);
  EXPECT_THROW(encode_receipt(LedgerReceipt{transaction.id, 0, 0, TransferResult::applied}), std::invalid_argument);
  EXPECT_THROW(encode_receipt(LedgerReceipt{transaction.id, 1, 1000, TransferResult::applied}), std::invalid_argument);
  auto bad = transaction; bad.signature[64] = 3;
  EXPECT_THROW(encode_batch(LedgerBatch{1, {bad}}), std::invalid_argument);
}
TEST_F(Records, BigEndianKeysSortNumerically) {
  for (const auto index : {1ULL, 255ULL, 256ULL, 65535ULL, 65536ULL}) EXPECT_EQ(decode_log_key(log_key(index)), index);
  EXPECT_LT(log_key(255), log_key(256)); EXPECT_LT(log_key(65535), log_key(65536));
  EXPECT_THROW(log_key(0), std::invalid_argument);
  EXPECT_THROW(decode_log_key("log/1"), std::invalid_argument);
  EXPECT_EQ(receipt_key(Hash{}).size(), 40U);
}
TEST_F(Records, RecoveryRejectsProgressSupplyRegistryAndGenesisMismatch) {
  LedgerState state{0, genesis.accounts}; EXPECT_NO_THROW(validate_recovery_state(genesis, state, 1));
  state.applied_index = 2; EXPECT_THROW(validate_recovery_state(genesis, state, 1), std::invalid_argument);
  state.applied_index = 1; state.accounts[0].balance = 99;
  EXPECT_THROW(validate_recovery_state(genesis, state, 1), std::invalid_argument);
  state = {1, genesis.accounts}; std::swap(state.accounts[0], state.accounts[1]);
  EXPECT_THROW(validate_recovery_state(genesis, state, 1), std::invalid_argument);
  state = {0, genesis.accounts}; state.accounts[0].nonce = 1;
  EXPECT_THROW(validate_recovery_state(genesis, state, 1), std::invalid_argument);
}
TEST_F(Records, OrderedBatchAndWithinBatchDuplicates) {
  const auto first = tx(), second = tx(1, 20);
  const auto plan = plan_application(genesis, {0, genesis.accounts}, 1, {1, {first, first, second}}, none);
  EXPECT_EQ(plan.results.size(), 3U); EXPECT_EQ(plan.new_receipts.size(), 2U);
  EXPECT_EQ(plan.results[0], plan.results[1]); EXPECT_EQ(plan.results[2].transaction_offset, 2U);
  EXPECT_EQ(plan.state.applied_index, 1U); EXPECT_EQ(plan.state.accounts[0].balance, 70U);
  EXPECT_EQ(plan.state.accounts[0].nonce, 2U); EXPECT_EQ(plan.state.accounts[1].balance, 30U);
}
TEST_F(Records, PriorReceiptsDoNotExecuteAgainOrChangeTheirPosition) {
  const auto first = tx(); const auto initial = plan_application(genesis, {0, genesis.accounts}, 1, {1, {first}}, none);
  const auto prior = [&](const Hash& id) -> std::optional<LedgerReceipt> {
    if (id == first.id) return initial.results[0]; return std::nullopt;
  };
  const auto next = plan_application(genesis, initial.state, 2, {2, {first, tx(1)}}, prior);
  EXPECT_EQ(next.results[0], initial.results[0]); EXPECT_EQ(next.new_receipts.size(), 1U);
  EXPECT_EQ(next.state.accounts[0].balance, 80U);
}
TEST_F(Records, RejectionsPreserveAccountsButAdvanceAppliedProgress) {
  const auto invalid_nonce = tx(5);
  const auto plan = plan_application(genesis, {0, genesis.accounts}, 1, {1, {invalid_nonce}}, none);
  EXPECT_EQ(plan.state.accounts, genesis.accounts); EXPECT_EQ(plan.state.applied_index, 1U);
  EXPECT_EQ(plan.results[0].result, TransferResult::wrong_nonce);
}
TEST_F(Records, UncommittedOutOfOrderAndConflictingReceiptsFail) {
  const auto transaction = tx(); const LedgerState state{0, genesis.accounts};
  EXPECT_THROW(plan_application(genesis, state, 0, {1, {transaction}}, none), std::invalid_argument);
  EXPECT_THROW(plan_application(genesis, state, 2, {2, {transaction}}, none), std::invalid_argument);
  const auto future = [&](const Hash&) { return std::optional<LedgerReceipt>{{transaction.id, 1, 0, TransferResult::applied}}; };
  EXPECT_THROW(plan_application(genesis, state, 1, {1, {transaction}}, future), std::invalid_argument);
  const auto mismatch = [&](const Hash&) { return std::optional<LedgerReceipt>{{Hash{}, 1, 0, TransferResult::applied}}; };
  EXPECT_THROW(plan_application(genesis, {1, genesis.accounts}, 2, {2, {transaction}}, mismatch), std::invalid_argument);
}
TEST_F(Records, InvalidLaterTransactionCannotPublishPartialApplication) {
  auto invalid = tx(1); invalid.id[0] ^= 1;
  const LedgerState state{0, genesis.accounts};
  EXPECT_THROW(plan_application(genesis, state, 1, {1, {tx(), invalid}}, none), std::invalid_argument);
  EXPECT_EQ(state.accounts, genesis.accounts); EXPECT_EQ(state.applied_index, 0U);
}
} // namespace
