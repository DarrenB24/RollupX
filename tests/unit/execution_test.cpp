#include "rollupx/execution.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <random>

namespace {
using namespace rollupx;
struct Execution : ::testing::Test {
  PrivateKey key = PrivateKey::from_bytes(fixed_hex<32>("0000000000000000000000000000000000000000000000000000000000000001"));
  Address recipient = fixed_hex<20>("3535353535353535353535353535353535353535");
  Accounts accounts{};
  void SetUp() override { accounts[0] = {address(key), 100, 0}; accounts[1] = {recipient, 10, 0}; }
  Transfer transfer() const { return {31337, address(key), recipient, 30, 0}; }
};
TEST_F(Execution, AppliesConservesSupplyAndReturnsExactRetries) {
  ExecutionState state(31337, accounts);
  const auto transaction = sign_transfer(key, transfer());
  EXPECT_EQ(state.apply(transaction), TransferResult::applied);
  EXPECT_EQ(state.accounts()[0].balance, 70U); EXPECT_EQ(state.accounts()[0].nonce, 1U);
  EXPECT_EQ(state.accounts()[1].balance, 40U); EXPECT_EQ(state.accounts()[1].nonce, 0U);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(state.apply(transaction), TransferResult::applied);
  EXPECT_EQ(state.accounts()[0].balance, 70U); EXPECT_EQ(state.receipt_count(), 1U);
  auto changed = transaction; changed.signature[0] ^= 1;
  EXPECT_THROW(state.apply(changed), std::invalid_argument);
  EXPECT_EQ(state.receipt_count(), 1U);
}
TEST_F(Execution, RejectsSemanticErrorsWithoutStateMutation) {
  const auto verify = [&](Transfer tx, TransferResult result) {
    const auto transition = execute_transfer(accounts, 31337, tx);
    EXPECT_EQ(transition.result, result); EXPECT_EQ(transition.accounts, accounts);
  };
  auto tx = transfer(); tx.chain_id = 1; verify(tx, TransferResult::wrong_chain);
  tx = transfer(); tx.amount = 0; verify(tx, TransferResult::zero_amount);
  tx = transfer(); tx.recipient = tx.sender; verify(tx, TransferResult::self_transfer);
  tx = transfer(); tx.recipient = {}; verify(tx, TransferResult::unknown_account);
  tx = transfer(); tx.nonce = 1; verify(tx, TransferResult::wrong_nonce);
  tx = transfer(); tx.amount = 101; verify(tx, TransferResult::insufficient_balance);
}
TEST_F(Execution, ChecksUint64BoundariesBeforeMutation) {
  auto tx = transfer();
  accounts[0].nonce = std::numeric_limits<std::uint64_t>::max(); tx.nonce = accounts[0].nonce;
  auto result = execute_transfer(accounts, 31337, tx);
  EXPECT_EQ(result.result, TransferResult::nonce_overflow); EXPECT_EQ(result.accounts, accounts);
  accounts[0].nonce = 0; tx.nonce = 0; accounts[1].balance = std::numeric_limits<std::uint64_t>::max();
  result = execute_transfer(accounts, 31337, tx);
  EXPECT_EQ(result.result, TransferResult::balance_overflow); EXPECT_EQ(result.accounts, accounts);
  // Defense in depth: such a total supply is also rejected during state restore.
  EXPECT_THROW(validate_accounts(accounts, false), std::invalid_argument);
}
TEST_F(Execution, GenesisValidationAndBoundedReceipts) {
  auto bad = accounts; bad[2] = bad[0]; EXPECT_THROW(ExecutionState(31337, bad), std::invalid_argument);
  bad = accounts; bad[2].balance = 1; EXPECT_THROW(ExecutionState(31337, bad), std::invalid_argument);
  bad = accounts; bad[0].nonce = 1; EXPECT_THROW(ExecutionState(31337, bad), std::invalid_argument);
  EXPECT_THROW(ExecutionState(0, accounts), std::invalid_argument);
  ExecutionState state(31337, accounts, 1);
  auto tx = transfer(); auto signed_tx = sign_transfer(key, tx); EXPECT_EQ(state.apply(signed_tx), TransferResult::applied);
  const auto before = state.accounts(); tx.nonce = 1;
  EXPECT_THROW(state.apply(sign_transfer(key, tx)), std::length_error);
  EXPECT_EQ(state.accounts(), before); EXPECT_EQ(state.receipt_count(), 1U);
  EXPECT_EQ(state.apply(signed_tx), TransferResult::applied);
}
TEST_F(Execution, RejectionReceiptsAreIdempotentAndOrderingIsDeterministic) {
  ExecutionState a(31337, accounts), b(31337, accounts);
  auto tx = transfer(); tx.nonce = 1; const auto future = sign_transfer(key, tx);
  EXPECT_EQ(a.apply(future), TransferResult::wrong_nonce); EXPECT_EQ(b.apply(future), TransferResult::wrong_nonce);
  const auto first = sign_transfer(key, transfer());
  EXPECT_EQ(a.apply(first), TransferResult::applied); EXPECT_EQ(b.apply(first), TransferResult::applied);
  EXPECT_EQ(a.apply(future), TransferResult::wrong_nonce); // Same rejected ID never changes its outcome.
  EXPECT_EQ(a.accounts(), b.accounts()); EXPECT_EQ(a.accounts()[0].nonce, 1U);
}
TEST_F(Execution, SeededTransferSequenceConservesSupply) {
  std::mt19937_64 random(493);
  for (int i = 0; i < 2000; ++i) {
    const std::size_t from = random()%2, to = 1-from;
    const auto amount = random()%150;
    const Transfer tx{31337, accounts[from].address, accounts[to].address, amount, accounts[from].nonce};
    const auto transition = execute_transfer(accounts, 31337, tx);
    if (transition.result == TransferResult::applied) {
      EXPECT_EQ(transition.accounts[from].nonce, accounts[from].nonce+1);
      EXPECT_EQ(transition.accounts[to].nonce, accounts[to].nonce);
    } else EXPECT_EQ(transition.accounts, accounts);
    accounts = transition.accounts;
    EXPECT_EQ(accounts[0].balance + accounts[1].balance, 110U);
    EXPECT_NO_THROW(validate_accounts(accounts, false));
  }
}
} // namespace
