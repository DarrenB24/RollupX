#include "rollupx/ledger_records.hpp"
#include "../ledger_fixture.hpp"
#include <gtest/gtest.h>
#include <map>

namespace {
using namespace rollupx;
using namespace rollupx::test;
// This is a model of atomic persisted images, not a replacement storage adapter.
// The same audit and application planner are used by the RocksDB implementation.
class Recovery : public ::testing::Test {
 protected:
  Genesis genesis = ledger_genesis();
  LedgerState state{0, genesis.accounts};
  std::map<Hash, LedgerReceipt> receipts;
  std::vector<LedgerBatch> history;
  BatchLookup batches = [this](std::uint64_t index) -> std::optional<LedgerBatch> {
    if (index == 0 || index > history.size()) return std::nullopt;
    return history[index-1];
  };
  ReceiptLookup lookup = [this](const Hash& id) -> std::optional<LedgerReceipt> {
    const auto found = receipts.find(id);
    return found == receipts.end() ? std::nullopt : std::optional{found->second};
  };
  void apply(const LedgerBatch& batch) {
    const auto plan = plan_application(genesis, state, history.size(), batch, lookup);
    for (const auto& receipt : plan.new_receipts) receipts.emplace(receipt.transaction_id, receipt);
    state = plan.state;
  }
  void audit() {
    RecoveryAudit check(genesis, state, history.size(), lookup, batches);
    for (const auto& batch : history) check.observe(batch.index, batch);
    check.finish();
  }
  void populate() {
    const auto first = ledger_transaction(), second = ledger_transaction(1, 20);
    const auto rejected = ledger_transaction(2, 1000);
    history = {{1, {first, first}}, {2, {second, first, rejected}}, {3, {rejected, ledger_transaction(2, 5)}}};
    for (const auto& batch : history) apply(batch);
  }
};
TEST_F(Recovery, AcceptsEmptyAndCompleteHistoryWithOriginalRetryReceipts) {
  EXPECT_NO_THROW(audit());
  populate(); EXPECT_NO_THROW(audit());
  EXPECT_EQ(state.accounts[0].balance, 65U); EXPECT_EQ(state.accounts[0].nonce, 3U);
  EXPECT_EQ(receipts.size(), 4U);
  EXPECT_EQ(receipts.at(history[2].transactions[0].id).log_index, 2U);
}
TEST_F(Recovery, EveryCommittedAppliedBoundaryConvergesWithoutDoubleExecution) {
  populate(); const auto final_state = state; const auto final_receipts = receipts;
  // Enumerate every crash image between commit and atomic application, including
  // repeated restarts at the same image and at every subsequent replay boundary.
  for (std::size_t committed = 0; committed <= history.size(); ++committed) {
    for (std::size_t applied = 0; applied <= committed; ++applied) {
      state = {0, genesis.accounts}; receipts.clear();
      for (std::size_t i = 0; i < applied; ++i) apply(history[i]);
      for (std::size_t replayed = applied; replayed <= committed; ++replayed) {
        for (int restart = 0; restart < 2; ++restart) {
          RecoveryAudit check(genesis, state, committed, lookup, batches);
          for (std::size_t i = 0; i < committed; ++i) check.observe(i+1, history[i]);
          ASSERT_NO_THROW(check.finish());
        }
        if (replayed < committed) apply(history[replayed]);
      }
      for (std::size_t i = committed; i < history.size(); ++i) apply(history[i]);
      EXPECT_EQ(state, final_state); EXPECT_EQ(receipts, final_receipts);
    }
  }
}
TEST_F(Recovery, RejectsMissingReorderedExtraAndMismatchedEntries) {
  populate();
  RecoveryAudit missing(genesis, state, history.size(), lookup, batches);
  missing.observe(1, history[0]); EXPECT_THROW(missing.finish(), std::runtime_error);
  RecoveryAudit gap(genesis, state, history.size(), lookup, batches);
  EXPECT_THROW(gap.observe(2, history[1]), std::invalid_argument);
  EXPECT_THROW(gap.observe(1, history[0]), std::logic_error); // Cannot skip a failure.
  RecoveryAudit mismatch(genesis, state, history.size(), lookup, batches);
  EXPECT_THROW(mismatch.observe(1, history[1]), std::invalid_argument);
  RecoveryAudit extra(genesis, {0, genesis.accounts}, 0, lookup, batches);
  EXPECT_THROW(extra.observe(1, history[0]), std::invalid_argument);
  RecoveryAudit repeated(genesis, state, history.size(), lookup, batches);
  repeated.observe(1, history[0]); EXPECT_THROW(repeated.observe(1, history[0]), std::invalid_argument);
}
TEST_F(Recovery, RejectsMissingAlteredAndMispositionedReceipts) {
  populate(); const auto original = receipts; const auto id = history[0].transactions[0].id;
  receipts.erase(id); EXPECT_THROW(audit(), std::runtime_error);
  receipts = original; receipts.at(id).result = TransferResult::wrong_nonce;
  EXPECT_THROW(audit(), std::runtime_error);
  receipts = original; receipts.at(id).transaction_offset = 1; // Same transaction, wrong first occurrence.
  EXPECT_THROW(audit(), std::runtime_error);
  receipts = original; receipts.at(id).log_index = 2;
  EXPECT_THROW(audit(), std::runtime_error);
  receipts = original; receipts.at(id).transaction_id[0] ^= 1;
  EXPECT_THROW(audit(), std::exception);
}
TEST_F(Recovery, RejectsSupplyPreservingBalanceAndNonceCorruption) {
  populate(); const auto original = state;
  --state.accounts[0].balance; ++state.accounts[1].balance;
  EXPECT_THROW(audit(), std::runtime_error);
  state = original; ++state.accounts[0].nonce;
  EXPECT_THROW(audit(), std::runtime_error);
}
TEST_F(Recovery, ForgedEarlierReceiptCannotSuppressFirstExecution) {
  history = {{1, {ledger_transaction(9)}}, {2, {ledger_transaction()}}};
  apply(history[0]);
  // Forge a previously applied receipt to suppress the first valid transfer and
  // pair it with a state image whose balances deliberately omit that transfer.
  const auto id = history[1].transactions[0].id;
  receipts.emplace(id, LedgerReceipt{id, 1, 0, TransferResult::applied});
  state.applied_index = 2;
  EXPECT_THROW(audit(), std::runtime_error);
}
TEST_F(Recovery, ValidatesPendingSuffixBeforeAnyReplay) {
  history = {{1, {ledger_transaction()}}, {2, {ledger_transaction(1)}}};
  history[1].transactions[0].id[0] ^= 1;
  EXPECT_THROW(audit(), std::invalid_argument);
  EXPECT_EQ(state.applied_index, 0U); EXPECT_TRUE(receipts.empty());
  history[1].transactions.clear(); EXPECT_THROW(audit(), std::invalid_argument);
}
TEST_F(Recovery, FinishedOrFailedAuditCannotBeReused) {
  RecoveryAudit complete(genesis, state, 0, lookup, batches); complete.finish();
  EXPECT_THROW(complete.finish(), std::logic_error);
  EXPECT_THROW(complete.observe(1, {1, {ledger_transaction()}}), std::logic_error);
  EXPECT_THROW(RecoveryAudit(genesis, state, 0, {}, batches), std::invalid_argument);
  history = {{1, {ledger_transaction()}}}; apply(history[0]);
  RecoveryAudit io_error(genesis, state, 1, [](const Hash&) -> std::optional<LedgerReceipt> {
    throw std::runtime_error("injected read error");
  }, batches);
  EXPECT_THROW(io_error.observe(1, history[0]), std::runtime_error);
  EXPECT_THROW(io_error.finish(), std::logic_error);
}
} // namespace
