#include "../snapshot_fixture.hpp"
#include "../test_support.hpp"
#include "rollupx/rocksdb_open.hpp"
#include <gtest/gtest.h>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
TEST(SnapshotStore, CaptureResumePublishAndReopenPreserveIdentityVotesAndOriginalResults) {
  Directory source_dir, target_dir; const auto genesis = ledger_genesis();
  const auto identity = raft_ledger_manifest(); auto other = identity; other.node_id = 2;
  std::string image;
  {
    Store source(source_dir.path,identity,genesis);
    auto state = ledger_snapshot().prefix; state.term = 6; state.voted_for = 3;
    state.log.push_back({6,encode_transfer_command({ledger_transaction(2,9)})});
    source.persist(state); source.apply_committed(); image = source.create_snapshot();
    EXPECT_EQ(source.load(),state); ASSERT_TRUE(source.local_snapshot()); EXPECT_EQ(*source.local_snapshot(),image);
  }
  {
    Store target(target_dir.path,other,genesis); target.persist({9,5,{},0});
    const auto progress = target.stage_snapshot(snapshot_chunk(image,0,100));
    EXPECT_EQ(progress.next_offset,100U); EXPECT_FALSE(progress.complete);
    EXPECT_EQ(target.stage_snapshot(snapshot_chunk(image,0,100)).next_offset,100U);
    EXPECT_EQ(target.ledger_state().applied_index,0U); EXPECT_FALSE(target.receipt(ledger_transaction().id));
  }
  {
    Store target(target_dir.path,other,genesis); ASSERT_TRUE(target.staged_snapshot());
    EXPECT_EQ(target.staged_snapshot()->next_offset,100U);
    EXPECT_THROW(target.publish_staged_snapshot(),std::invalid_argument);
    EXPECT_TRUE(target.stage_snapshot(snapshot_chunk(image,100,image.size()-100)).complete);
    EXPECT_EQ(target.load().commit,0U);
    ASSERT_TRUE(target.publish_staged_snapshot()); EXPECT_FALSE(target.staged_snapshot());
    EXPECT_EQ(target.load().term,9U); EXPECT_EQ(target.load().voted_for,5U);
    EXPECT_EQ(target.load().commit,4U); EXPECT_EQ(target.ledger_state(),ledger_snapshot().ledger);
    for (const auto& [id,receipt] : ledger_snapshot().receipts) {
      ASSERT_TRUE(target.receipt(id)); EXPECT_EQ(*target.receipt(id),receipt);
    }
    ASSERT_TRUE(target.batch(2)); EXPECT_EQ(target.batch(2)->transactions.size(),2U);
    stage_image(target,image); EXPECT_FALSE(target.publish_staged_snapshot());
    EXPECT_EQ(target.load().term,9U); EXPECT_EQ(target.load().voted_for,5U);
  }
  Store reopened(target_dir.path,other,genesis);
  EXPECT_EQ(reopened.ledger_state(),ledger_snapshot().ledger); EXPECT_EQ(reopened.local_snapshot(),image);
  Store source_reopened(source_dir.path,identity,genesis);
  EXPECT_EQ(source_reopened.load().log.size(),5U); EXPECT_EQ(source_reopened.load().commit,4U);
}
TEST(SnapshotStore, MatchingSuffixIsPreservedAndLaterAppliedExactlyOnce) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  auto state = ledger_snapshot().prefix; state.term = 5; state.voted_for = 3; state.commit = 2;
  state.log.push_back({2,encode_transfer_command({ledger_transaction(2,5)})});
  store.persist(state); store.apply_committed(); stage_image(store,snapshot_image());
  ASSERT_TRUE(store.publish_staged_snapshot());
  state.commit = 4; EXPECT_EQ(store.load(),state); EXPECT_EQ(store.ledger_state(),ledger_snapshot().ledger);
  state.commit = 5; store.persist(state); EXPECT_EQ(store.apply_committed(),5U);
  EXPECT_EQ(store.ledger_state().accounts[0].balance,65U); EXPECT_EQ(store.ledger_state().accounts[0].nonce,3U);
  EXPECT_EQ(store.receipt(ledger_transaction().id)->log_index,2U);
}
TEST(SnapshotStore, DivergentUncommittedSuffixIsReplacedWithoutRewritingCommittedHistory) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  auto state = ledger_snapshot().prefix; state.term = 5; state.voted_for = 4; state.commit = 2;
  state.log.resize(2); state.log.push_back({2,encode_transfer_command({ledger_transaction(1,9)})});
  state.log.push_back({2,{}}); state.log.push_back({3,{}});
  store.persist(state); store.apply_committed(); stage_image(store,snapshot_image());
  ASSERT_TRUE(store.publish_staged_snapshot());
  EXPECT_EQ(store.load().log,ledger_snapshot().prefix.log); EXPECT_EQ(store.load().term,5U); EXPECT_EQ(store.load().voted_for,4U);
  EXPECT_FALSE(store.receipt(ledger_transaction(1,9).id)); EXPECT_EQ(store.ledger_state(),ledger_snapshot().ledger);
}
TEST(SnapshotStore, TermPreconditionAndProgressRevalidationFailWithoutPoisoningTheLedger) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
  stage_image(store,snapshot_image());
  EXPECT_THROW(store.publish_staged_snapshot(),std::invalid_argument); EXPECT_EQ(store.load().term,0U);
  store.persist({3,2,{},0}); ASSERT_TRUE(store.publish_staged_snapshot());
  stage_image(store,snapshot_image());
  auto later = store.load(); later.log.push_back({3,encode_transfer_command({ledger_transaction(2,7)})}); later.commit = 5;
  store.persist(later); store.apply_committed();
  EXPECT_THROW(store.publish_staged_snapshot(),std::invalid_argument);
  EXPECT_EQ(store.load(),later); EXPECT_EQ(store.ledger_state().accounts[0].balance,63U);
  store.discard_staged_snapshot(); EXPECT_FALSE(store.staged_snapshot());
}
TEST(SnapshotStore, ConflictingCommittedPrefixAndSameBoundaryTermWithDifferentBytesAreRejected) {
  for (const auto committed : {Index{2},Index{0}}) {
    SCOPED_TRACE(committed); Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis());
    auto state = ledger_snapshot().prefix; state.term = 3; state.commit = committed;
    state.log[1].command = encode_transfer_command({ledger_transaction(0,11)});
    store.persist(state); store.apply_committed(); stage_image(store,snapshot_image());
    EXPECT_THROW(store.publish_staged_snapshot(),std::invalid_argument);
    EXPECT_EQ(store.load(),state); EXPECT_EQ(store.ledger_state().applied_index,committed);
  }
}
TEST(SnapshotStore, InvalidChunksAndForeignCompleteImageLeaveActiveAndStagedDataUnchanged) {
  Directory directory; Store store(directory.path,raft_ledger_manifest(),ledger_genesis()); store.persist({3,2,{},0});
  const auto image = snapshot_image(); store.stage_snapshot(snapshot_chunk(image,0,100));
  auto chunk = snapshot_chunk(image,0,100); chunk.data[1] ^= 1;
  EXPECT_THROW(store.stage_snapshot(chunk),std::invalid_argument);
  EXPECT_THROW(store.stage_snapshot(snapshot_chunk(image,101,10)),std::invalid_argument);
  auto foreign = raft_ledger_manifest(); foreign.cluster_id = "foreign";
  const auto other = encode_snapshot(foreign,ledger_genesis(),ledger_snapshot());
  EXPECT_THROW(stage_image(store,other),std::invalid_argument);
  EXPECT_EQ(store.staged_snapshot()->next_offset,100U); EXPECT_EQ(store.load().commit,0U);
  EXPECT_EQ(store.ledger_state().accounts,ledger_genesis().accounts);
  store.discard_staged_snapshot(); EXPECT_FALSE(store.staged_snapshot());
}
TEST(SnapshotStore, AmbiguousPublicationFailurePoisonsHandleAndReopenRecoversWholeGeneration) {
  Directory directory;
  {
    Store store(directory.path,raft_ledger_manifest(),ledger_genesis(),[](StoreCrashPoint point) {
      if (point == StoreCrashPoint::after_publish) throw std::runtime_error("ambiguous snapshot write");
    });
    store.persist({3,2,{},0}); stage_image(store,snapshot_image());
    EXPECT_THROW(store.publish_staged_snapshot(),std::runtime_error);
    EXPECT_THROW(store.load(),std::runtime_error);
    EXPECT_THROW(store.receipt(ledger_transaction().id),std::runtime_error);
  }
  Store reopened(directory.path,raft_ledger_manifest(),ledger_genesis());
  EXPECT_EQ(reopened.load().commit,4U); EXPECT_EQ(reopened.load().voted_for,2U);
  EXPECT_EQ(reopened.ledger_state(),ledger_snapshot().ledger); EXPECT_FALSE(reopened.staged_snapshot());
  EXPECT_EQ(reopened.local_snapshot(),snapshot_image());
}
TEST(SnapshotStore, DamagedStagingNeverBecomesActiveAndCanBeDiscardedAfterReopen) {
  Directory directory;
  { Store store(directory.path,raft_ledger_manifest(),ledger_genesis()); store.persist({3,2,{},0}); }
  {
    std::unique_ptr<rocksdb::DB> db; ASSERT_TRUE(open_rocksdb(rocksdb::Options{},directory.path,db).ok());
    rocksdb::WriteOptions options; options.sync = true;
    ASSERT_TRUE(db->Put(options,"snapshot/staging","corrupt").ok());
  }
  {
    Store reopened(directory.path,raft_ledger_manifest(),ledger_genesis());
    EXPECT_EQ(reopened.ledger_state().applied_index,0U);
    EXPECT_THROW(reopened.staged_snapshot(),std::invalid_argument);
    EXPECT_THROW(reopened.load(),std::runtime_error);
  }
  Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
  recovered.discard_staged_snapshot(); EXPECT_FALSE(recovered.staged_snapshot()); EXPECT_EQ(recovered.load().term,3U);
}
} // namespace
