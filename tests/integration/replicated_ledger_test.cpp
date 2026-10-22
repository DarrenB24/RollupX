#include "../test_support.hpp"
#include "../raft_store_fixture.hpp"
#include "rollupx/raft_runtime.hpp"
#include "rollupx/ledger_submission.hpp"
#include <gtest/gtest.h>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
TEST(ReplicatedLedger, CommitAndApplicationAreSeparateAndNoopsHaveDurableProgress) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  store.persist(raft_ledger_history()); EXPECT_EQ(store.load().commit,4U);
  EXPECT_EQ(store.ledger_state().applied_index,0U); EXPECT_FALSE(store.receipt(ledger_transaction().id));
  EXPECT_EQ(store.apply_committed(),4U); EXPECT_EQ(store.ledger_state().accounts[0].balance,70U);
  EXPECT_EQ(store.ledger_state().accounts[0].nonce,2U);
  ASSERT_TRUE(store.receipt(ledger_transaction().id)); EXPECT_EQ(store.receipt(ledger_transaction().id)->log_index,2U);
  EXPECT_FALSE(store.batch(1)); EXPECT_FALSE(store.batch(3)); ASSERT_TRUE(store.batch(2));
  EXPECT_EQ(store.batch(2)->transactions.size(),2U); EXPECT_EQ(store.apply_committed(),4U);
}
TEST(ReplicatedLedger, ReopenReplaysCommittedSuffixButNeverUncommittedEntries) {
  Directory directory;
  {
    Store store(directory.path,raft_ledger_manifest(),ledger_genesis()); auto history = raft_ledger_history();
    history.commit = 2; store.persist(history);
  }
  {
    Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
    EXPECT_EQ(recovered.ledger_state().applied_index,2U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,90U);
    EXPECT_FALSE(recovered.batch(4)); recovered.persist(raft_ledger_history());
  }
  Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
  EXPECT_EQ(recovered.ledger_state().applied_index,4U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,70U);
  const auto rejected = recovered.receipt(ledger_transaction(1,1000).id);
  ASSERT_TRUE(rejected);
  EXPECT_EQ(rejected->result,TransferResult::insufficient_balance);
}
TEST(ReplicatedLedger, ApplicationFailurePoisonsHandleAndReopenResolvesOutcome) {
  for (const auto point : {StoreCrashPoint::before_apply,StoreCrashPoint::after_apply}) {
    Directory directory;
    {
      Store store(directory.path,raft_ledger_manifest(),ledger_genesis(),[point](StoreCrashPoint current) {
        if (current == point) throw std::runtime_error("application interrupted");
      });
      store.persist(raft_ledger_history()); EXPECT_THROW(store.apply_committed(),std::runtime_error);
      EXPECT_THROW(store.ledger_state(),std::runtime_error);
      EXPECT_THROW(store.receipt(ledger_transaction().id),std::runtime_error);
    }
    Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
    EXPECT_EQ(recovered.ledger_state().applied_index,4U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,70U);
  }
}
TEST(ReplicatedLedger, CorePersistenceCallbackAndDurableApplicationAgree) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  Core core(raft_config(),store.load(),[&](const State& state) { store.persist(state); });
  const auto history = raft_ledger_history();
  const auto responses = core.receive({2,1,AppendRequest{1,1,0,0,history.log,history.commit}},0);
  ASSERT_TRUE(std::get<AppendResponse>(responses[0].message).success);
  EXPECT_EQ(store.ledger_state().applied_index,0U); EXPECT_EQ(store.apply_committed(),core.state().commit);
  EXPECT_EQ(store.ledger_state().accounts[0].balance,70U);
}
TEST(ReplicatedLedger, ModeAndGenesisMismatchNeverSilentlyInitializeOrUpgrade) {
  Directory core_directory; Directory ledger_directory;
  { Store core_only(core_directory.path,raft_ledger_manifest()); EXPECT_THROW(core_only.apply_committed(),std::logic_error); }
  EXPECT_THROW({ Store wrong(core_directory.path,raft_ledger_manifest(),ledger_genesis()); },std::runtime_error);
  { Store ledger(ledger_directory.path,raft_ledger_manifest(),ledger_genesis()); }
  EXPECT_THROW({ Store wrong(ledger_directory.path,raft_ledger_manifest()); },std::runtime_error);
  auto changed = ledger_genesis(); ++changed.chain_id;
  EXPECT_THROW({ Store wrong(ledger_directory.path,raft_ledger_manifest(),changed); },std::invalid_argument);
}
TEST(ReplicatedLedger, MalformedCommandDoesNotEnterRaftDatabase) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  auto bad = raft_ledger_history(); bad.log[3].command[10] ^= 1;
  EXPECT_THROW(store.persist(bad),std::invalid_argument);
  EXPECT_EQ(store.load(),State{}); EXPECT_EQ(store.ledger_state().applied_index,0U);
  EXPECT_NO_THROW(store.persist(raft_ledger_history()));
}
TEST(ReplicatedLedger, NamedMembershipSurvivesReplayAndCannotBeRenamedOrDowngraded) {
  Directory directory; auto identity = raft_ledger_manifest();
  identity.member_names = {"n1","n2","n3","n4","n5"};
  { Store store(directory.path,identity,ledger_genesis()); store.persist(raft_ledger_history()); }
  auto renamed = identity; renamed.member_names[1] = "n2-renamed";
  EXPECT_THROW({ Store wrong(directory.path,renamed,ledger_genesis()); },std::runtime_error);
  EXPECT_THROW({ Store legacy(directory.path,raft_ledger_manifest(),ledger_genesis()); },std::runtime_error);
  Store recovered(directory.path,identity,ledger_genesis());
  EXPECT_EQ(recovered.apply_committed(),4U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,70U);
}
TEST(ReplicatedLedger, ExistingNumericOnlyStoreCannotBeUpgradedBySupplyingNames) {
  Directory directory; const auto identity = raft_ledger_manifest();
  { Store legacy(directory.path,identity,ledger_genesis()); legacy.persist(raft_ledger_history()); }
  auto named = identity; named.member_names = {"n1","n2","n3","n4","n5"};
  EXPECT_THROW({ Store wrong(directory.path,named,ledger_genesis()); },std::runtime_error);
  Store unchanged(directory.path,identity,ledger_genesis());
  EXPECT_EQ(unchanged.apply_committed(),4U); EXPECT_EQ(unchanged.ledger_state().accounts[0].balance,70U);
}
TEST(ReplicatedLedger, RuntimePublishesReplyOnlyAfterStoreApplicationAndSurvivesReopen) {
  Directory directory;
  const auto identity = make_named_manifest("test-cluster",{"n1","n2","n3","n4","n5"},
    raft_ledger_manifest().genesis_digest,1);
  const auto history = raft_ledger_history();
  for (int restart = 0; restart < 2; ++restart) {
    Store store(directory.path,identity,ledger_genesis());
    Runtime runtime(raft_config(),store.load(),[&](const State& state) { store.persist(state); },
      [&](Index) { return store.apply_committed(); },[](const Envelope&) { return true; },
      {32,1024*1024,false,std::chrono::milliseconds(10)});
    auto request = runtime.handle().receive({2,1,AppendRequest{1,1,0,0,history.log,history.commit}});
    ASSERT_EQ(request.admission,Admission::queued);
    ASSERT_EQ(request.result.wait_for(std::chrono::seconds(3)),std::future_status::ready);
    const auto result = request.result.get(); ASSERT_TRUE(result.reply);
    EXPECT_EQ(result.completion,Completion::processed); EXPECT_EQ(result.status.applied,4U);
    EXPECT_TRUE(std::get<AppendResponse>(result.reply->message).success);
    EXPECT_EQ(store.ledger_state().accounts[0].balance,70U); EXPECT_EQ(store.ledger_state().accounts[0].nonce,2U);
    runtime.stop();
  }
}
TEST(ReplicatedLedger, SubmissionRetriesReturnDurableSuccessAndRejectionAcrossReopen) {
  using namespace std::chrono_literals;
  Directory directory; const auto identity = raft_ledger_manifest();
  { Store store(directory.path,identity,ledger_genesis()); store.persist(raft_ledger_history()); }
  for (unsigned reopen = 0; reopen < 2; ++reopen) {
    Store store(directory.path,identity,ledger_genesis());
    const ReceiptLookup read = [&](const Hash& id) { return store.receipt(id); };
    Runtime runtime(raft_config(),store.load(),[&](const State& state) { store.persist(state); },
      [&](Index) { return store.apply_committed(); },[](const Envelope&) { return false; },
      RuntimeOptions{32,1024*1024,false,10ms},{},
      ledger_proposal_policy(ledger_genesis(),[&] { return store.ledger_state(); },read));
    LedgerSubmission submission(runtime.handle(),read);
    for (const auto& transaction : {ledger_transaction(),ledger_transaction(1,1000)}) {
      const auto result = submission.submit(transaction,std::chrono::steady_clock::now()+1s);
      EXPECT_EQ(result.code,SubmissionCode::confirmed); EXPECT_EQ(result.receipt,store.receipt(transaction.id));
    }
    EXPECT_EQ(store.ledger_state().accounts[0].balance,70U);
    EXPECT_EQ(store.ledger_state().accounts[0].nonce,2U); EXPECT_EQ(store.load().log.size(),4U);
    submission.close(); runtime.stop();
  }
}
} // namespace
