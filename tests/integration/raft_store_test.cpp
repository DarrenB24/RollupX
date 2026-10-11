#include "../test_support.hpp"
#include "../raft_store_fixture.hpp"
#include <gtest/gtest.h>
#include <rocksdb/db.h>

namespace {
using namespace rollupx::raft;
using namespace rollupx::test;
TEST(RaftStore, CoreVoteAppendAndCommitSurviveReopen) {
  Directory directory; const auto manifest = raft_manifest();
  {
    Store store(directory.path,manifest);
    Core core(raft_config(),store.load(),[&](const State& state) { store.persist(state); });
    const auto vote = core.receive({2,1,VoteRequest{1,0,0}},0);
    ASSERT_TRUE(std::get<VoteResponse>(vote[0].message).granted);
    const auto appended = core.receive({2,1,AppendRequest{1,1,0,0,{{1,"committed"},{1,"pending"}},1}},1);
    ASSERT_TRUE(std::get<AppendResponse>(appended[0].message).success);
  }
  Store recovered(directory.path,manifest,{},StoreOpenMode::existing_only);
  Core core(raft_config(),recovered.load(),[&](const State& state) { recovered.persist(state); });
  EXPECT_EQ(core.state().voted_for,2U); EXPECT_EQ(core.state().commit,1U); ASSERT_EQ(core.state().log.size(),2U);
  const auto vote = core.receive({3,1,VoteRequest{1,2,1}},2);
  EXPECT_FALSE(std::get<VoteResponse>(vote[0].message).granted);
}
TEST(RaftStore, IdentityExclusiveLockAndExistingOnlyAreEnforced) {
  Directory directory; auto manifest = raft_manifest();
  {
    Store store(directory.path,manifest);
    EXPECT_THROW({ Store other(directory.path,manifest); },std::runtime_error);
  }
  manifest.node_id = 2; EXPECT_THROW({ Store other(directory.path,manifest); },std::runtime_error);
  manifest = raft_manifest(); manifest.genesis_digest[0] ^= 1;
  EXPECT_THROW({ Store other(directory.path,manifest); },std::runtime_error);
  EXPECT_THROW({ Store missing(directory.path+"/absent",raft_manifest(),{},StoreOpenMode::existing_only); },std::runtime_error);
}
TEST(RaftStore, InvalidUpdateDoesNotPoisonOrModifyDurableState) {
  Directory directory; Store store(directory.path,raft_manifest());
  const State state{2,2,{{1,"kept"}},1}; store.persist(state);
  auto invalid = state; invalid.voted_for = 3;
  EXPECT_THROW(store.persist(invalid),std::invalid_argument); EXPECT_EQ(store.load(),state);
  invalid = state; invalid.log[0].command = "changed";
  EXPECT_THROW(store.persist(invalid),std::invalid_argument); EXPECT_EQ(store.load(),state);
  auto next = state; next.term = 3; next.voted_for.reset();
  EXPECT_NO_THROW(store.persist(next)); EXPECT_EQ(store.load(),next);
}
TEST(RaftStore, AmbiguousFailuresRequireReopenAndReturnOnlyCompleteImages) {
  for (const auto point : {StoreCrashPoint::before_write,StoreCrashPoint::after_write}) {
    Directory directory; const State next{1,2,{{1,"entry"}},1};
    {
      Store store(directory.path,raft_manifest(),[point](StoreCrashPoint current) {
        if (point == current) throw std::runtime_error("injected interruption");
      });
      EXPECT_THROW(store.persist(next),std::runtime_error);
      EXPECT_THROW(store.load(),std::runtime_error);
      EXPECT_THROW(store.persist(next),std::runtime_error);
    }
    Store recovered(directory.path,raft_manifest());
    EXPECT_EQ(recovered.load(),point == StoreCrashPoint::after_write ? next : State{});
  }
}
TEST(RaftStore, RepairedSuffixPersistsAndCommittedPrefixRemains) {
  Directory directory; const State before{2,{},{{1,"committed"},{1,"divergent"},{1,"tail"}},1};
  const State after{2,{},{{1,"committed"},{2,"replacement"}},2};
  { Store store(directory.path,raft_manifest()); store.persist(before); store.persist(after); }
  Store recovered(directory.path,raft_manifest()); EXPECT_EQ(recovered.load(),after);
}
void damage(const std::string& directory, const std::string& key, const std::optional<std::string>& value) {
  rocksdb::DB* opened = nullptr; const auto status = rocksdb::DB::Open(rocksdb::Options{},directory,&opened);
  const std::unique_ptr<rocksdb::DB> db(opened); ASSERT_TRUE(status.ok()) << status.ToString();
  rocksdb::WriteOptions options; options.sync = true;
  const auto result = value ? db->Put(options,key,*value) : db->Delete(options,key);
  ASSERT_TRUE(result.ok()) << result.ToString();
}
TEST(RaftStore, MissingCorruptAndForeignRecordsFailClosed) {
  const auto identity = raft_manifest();
  for (const auto* scenario : {"missing-state","missing-manifest","corrupt-state","foreign-state"}) {
    SCOPED_TRACE(scenario); Directory directory;
    { Store store(directory.path,identity); store.persist({1,2,{{1,"entry"}},1}); }
    if (std::string_view(scenario) == "missing-manifest") damage(directory.path,"meta/identity",{});
    else if (std::string_view(scenario) == "missing-state") damage(directory.path,"raft/state",{});
    else {
      auto manifest = identity;
      if (std::string_view(scenario) == "foreign-state") manifest.node_id = 2;
      auto record = encode_state_record(manifest,{1,2,{{1,"entry"}},1});
      if (std::string_view(scenario) == "corrupt-state") record[40] ^= 1;
      damage(directory.path,"raft/state",record);
    }
    EXPECT_THROW({ Store recovered(directory.path,identity); },std::exception);
  }
}
} // namespace
