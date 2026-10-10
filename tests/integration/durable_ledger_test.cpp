#include "../test_support.hpp"
#include "../ledger_fixture.hpp"
#include <future>
#include <gtest/gtest.h>
#include <rocksdb/db.h>

namespace {
using namespace rollupx;
using namespace rollupx::test;
TEST(DurableLedger, ReopenPreservesResultsAndExactRetryDoesNotAppend) {
  Directory directory; const auto genesis = ledger_genesis(); const auto transaction = ledger_transaction();
  LedgerReceipt original{};
  {
    DurableLedger ledger(directory.path, genesis); original = ledger.submit(transaction);
    EXPECT_EQ(original.result, TransferResult::applied); EXPECT_EQ(original.log_index, 1U);
    EXPECT_EQ(ledger.snapshot().accounts[0].balance, 90U);
  }
  DurableLedger reopened(directory.path, genesis);
  EXPECT_EQ(reopened.submit(transaction), original); EXPECT_EQ(reopened.receipt(transaction.id), original);
  EXPECT_EQ(reopened.snapshot().applied_index, 1U); EXPECT_EQ(reopened.snapshot().accounts[0].nonce, 1U);
  ASSERT_TRUE(reopened.batch(1)); EXPECT_EQ(reopened.batch(1)->transactions[0], transaction);
  EXPECT_FALSE(reopened.batch(2));
}
TEST(DurableLedger, DuplicateConcurrentRequestsHaveOneDurableEffect) {
  Directory directory; DurableLedger ledger(directory.path, ledger_genesis());
  const auto transaction = ledger_transaction(); std::vector<std::future<LedgerReceipt>> jobs;
  for (int i = 0; i < 16; ++i) jobs.push_back(std::async(std::launch::async, [&] { return ledger.submit(transaction); }));
  for (auto& job : jobs) EXPECT_EQ(job.get().log_index, 1U);
  EXPECT_EQ(ledger.snapshot().applied_index, 1U); EXPECT_EQ(ledger.snapshot().accounts[0].balance, 90U);
}
TEST(DurableLedger, BatchDeduplicationAndRejectionAreAtomicAcrossReopen) {
  Directory directory; const auto genesis = ledger_genesis(); const auto first = ledger_transaction();
  const auto second = ledger_transaction(1, 20), rejected = ledger_transaction(2, 1000);
  {
    DurableLedger ledger(directory.path, genesis);
    const auto receipts = ledger.commit_development_batch({first, first, second, rejected});
    EXPECT_EQ(receipts.size(), 4U); EXPECT_EQ(receipts[0], receipts[1]);
    EXPECT_EQ(receipts[2].transaction_offset, 2U); EXPECT_EQ(receipts[3].result, TransferResult::insufficient_balance);
  }
  DurableLedger reopened(directory.path, genesis);
  EXPECT_EQ(reopened.snapshot().accounts[0].balance, 70U); EXPECT_EQ(reopened.snapshot().accounts[0].nonce, 2U);
  EXPECT_EQ(reopened.submit(rejected).result, TransferResult::insufficient_balance);
  EXPECT_EQ(reopened.snapshot().applied_index, 1U);
}
TEST(DurableLedger, MalformedBatchNeverCommitsItsValidPrefix) {
  Directory directory; DurableLedger ledger(directory.path, ledger_genesis());
  const auto valid = ledger_transaction(); auto invalid = ledger_transaction(1); invalid.id[0] ^= 1;
  EXPECT_THROW(ledger.commit_development_batch({valid, invalid}), std::invalid_argument);
  EXPECT_EQ(ledger.snapshot().applied_index, 0U); EXPECT_FALSE(ledger.receipt(valid.id));
  EXPECT_EQ(ledger.submit(valid).result, TransferResult::applied); // Admission error did not poison DB.
}
TEST(DurableLedger, FailedCommitOutcomeRequiresReopenAndReplays) {
  Directory directory; const auto genesis = ledger_genesis(); const auto transaction = ledger_transaction();
  {
    DurableLedger ledger(directory.path, genesis, [](LedgerCrashPoint point) {
      if (point == LedgerCrashPoint::after_commit) throw std::runtime_error("injected interruption");
    });
    EXPECT_THROW(ledger.submit(transaction), std::runtime_error);
    EXPECT_THROW(ledger.snapshot(), std::runtime_error);
    EXPECT_THROW(ledger.submit(transaction), std::runtime_error);
  }
  DurableLedger recovered(directory.path, genesis);
  EXPECT_EQ(recovered.snapshot().accounts[0].balance, 90U); EXPECT_EQ(recovered.submit(transaction).log_index, 1U);
}
TEST(DurableLedger, RejectsWrongGenesisAndExclusiveOwnership) {
  Directory directory; const auto genesis = ledger_genesis();
  {
    DurableLedger first(directory.path, genesis);
    EXPECT_THROW({ DurableLedger second(directory.path, genesis); }, std::runtime_error);
  }
  auto changed = genesis; ++changed.chain_id;
  EXPECT_THROW({ DurableLedger ledger(directory.path, changed); }, std::runtime_error);
  changed = genesis; ++changed.accounts[0].balance;
  EXPECT_THROW({ DurableLedger ledger(directory.path, changed); }, std::runtime_error);
  EXPECT_THROW({ DurableLedger ledger(directory.path+"/missing", genesis, {}, LedgerOpenMode::existing_only); }, std::runtime_error);
}
void corrupt(const std::string& directory, const std::string& key, const std::optional<std::string>& value) {
  rocksdb::DB* output = nullptr;
  const auto opened = rocksdb::DB::Open(rocksdb::Options{}, directory, &output);
  const std::unique_ptr<rocksdb::DB> db(output); ASSERT_TRUE(opened.ok()) << opened.ToString();
  rocksdb::WriteOptions options; options.sync = true;
  const auto written = value ? db->Put(options, key, *value) : db->Delete(options, key);
  ASSERT_TRUE(written.ok()) << written.ToString();
}
TEST(DurableLedger, MissingCommittedHistoryFailsClosed) {
  Directory directory; const auto genesis = ledger_genesis();
  { DurableLedger ledger(directory.path, genesis); (void)ledger.submit(ledger_transaction()); }
  corrupt(directory.path, log_key(1), std::nullopt);
  EXPECT_THROW({ DurableLedger ledger(directory.path, genesis); }, std::runtime_error);
}
TEST(DurableLedger, MissingReceiptOrInconsistentStateIsNotSilentlyRepaired) {
  const auto genesis = ledger_genesis(); const auto transaction = ledger_transaction();
  {
    Directory directory;
    { DurableLedger ledger(directory.path, genesis); (void)ledger.submit(transaction); }
    corrupt(directory.path, receipt_key(transaction.id), std::nullopt);
    EXPECT_THROW({ DurableLedger ledger(directory.path, genesis); }, std::runtime_error);
  }
  {
    Directory directory; LedgerState snapshot{};
    { DurableLedger ledger(directory.path, genesis); (void)ledger.submit(transaction); snapshot = ledger.snapshot(); }
    ++snapshot.accounts[0].nonce;
    corrupt(directory.path, "ledger/state", encode_state(snapshot));
    EXPECT_THROW({ DurableLedger ledger(directory.path, genesis); }, std::runtime_error);
  }
}
TEST(DurableLedger, ForgedReceiptCannotSuppressPendingTransferOnRecovery) {
  Directory directory; const auto genesis = ledger_genesis(); const auto pending = ledger_transaction();
  {
    DurableLedger ledger(directory.path, genesis);
    EXPECT_EQ(ledger.submit(ledger_transaction(9)).result, TransferResult::wrong_nonce);
  }
  {
    DurableLedger ledger(directory.path, genesis, [](LedgerCrashPoint point) {
      if (point == LedgerCrashPoint::after_commit) throw std::runtime_error("leave pending transfer");
    });
    EXPECT_THROW(ledger.submit(pending), std::runtime_error);
  }
  corrupt(directory.path, receipt_key(pending.id),
    encode_receipt({pending.id, 1, 0, TransferResult::applied}));
  EXPECT_THROW({ DurableLedger ledger(directory.path, genesis); }, std::runtime_error);
}
} // namespace
