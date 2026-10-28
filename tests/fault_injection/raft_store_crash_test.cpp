#include "../test_support.hpp"
#include "../raft_store_fixture.hpp"
#include "../snapshot_fixture.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <gtest/gtest.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace {
using namespace rollupx::raft;
using namespace rollupx::test;
bool crash(const std::string& directory, const char* boundary, const char* operation,
    const char* worker = ROLLUPX_RAFT_CRASH_WORKER) {
  std::vector<std::string> values{worker,directory,boundary,operation};
  std::vector<char*> args; for (auto& value : values) args.push_back(value.data()); args.push_back(nullptr);
  pid_t pid = -1;
  if (::posix_spawn(&pid,values[0].c_str(),nullptr,nullptr,args.data(),environ) != 0) return false;
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(pid,&status,WNOHANG);
    if (result == pid) return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    if (result < 0 && errno != EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(pid,SIGKILL); while (::waitpid(pid,&status,0) < 0 && errno == EINTR) {}
  return false;
}
TEST(RaftStoreCrash, KillBeforeAndAfterEveryPersistentProtocolEffect) {
  const State initial{1,{},{{1,"seed"}},0};
  const std::vector<std::pair<std::string,State>> operations{
    {"term",{2,{},{{1,"seed"}},0}}, {"vote",{1,2,{{1,"seed"}},0}},
    {"append",{1,{},{{1,"seed"},{1,"append"}},0}}, {"commit",{1,{},{{1,"seed"}},1}},
    {"replace",{2,{},{{2,"replacement"}},0}}};
  for (const auto& [operation,after] : operations) for (const auto* boundary : {"before","after"}) {
    SCOPED_TRACE(operation+"/"+boundary); Directory directory;
    { Store store(directory.path,raft_manifest()); store.persist(initial); }
    ASSERT_TRUE(crash(directory.path,boundary,operation.c_str()));
    const auto expected = std::string_view(boundary) == "before" ? initial : after;
    for (int restart = 0; restart < 2; ++restart) {
      Store recovered(directory.path,raft_manifest(),{},StoreOpenMode::existing_only);
      EXPECT_EQ(recovered.load(),expected);
      if (operation == "vote" && std::string_view(boundary) == "after") {
        Core core(raft_config(),recovered.load(),[&](const State& state) { recovered.persist(state); });
        const auto reply = core.receive({3,1,VoteRequest{1,1,1}},0);
        EXPECT_FALSE(std::get<VoteResponse>(reply[0].message).granted);
      }
    }
  }
}
TEST(RaftApplicationCrash, KilledCommitAndApplicationRecoverAtomicLedgerEffects) {
  for (const auto* point : {"after_commit","before_apply","after_apply"}) {
    SCOPED_TRACE(point); Directory directory;
    { Store initial(directory.path,raft_ledger_manifest(),ledger_genesis()); }
    const auto* occurrence = std::string_view(point) == "after_commit" ? "1" : "2";
    ASSERT_TRUE(crash(directory.path,point,occurrence,ROLLUPX_APPLICATION_CRASH_WORKER));
    Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
    EXPECT_EQ(recovered.ledger_state().applied_index,4U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,70U);
    EXPECT_EQ(recovered.ledger_state().accounts[0].nonce,2U);
    ASSERT_TRUE(recovered.receipt(ledger_transaction().id)); EXPECT_EQ(recovered.receipt(ledger_transaction().id)->log_index,2U);
  }
}
TEST(RaftApplicationCrash, RepeatedProcessKillsDuringReplayPreserveExactlyOnceEffects) {
  Directory directory;
  { Store initial(directory.path,raft_ledger_manifest(),ledger_genesis()); }
  ASSERT_TRUE(crash(directory.path,"after_commit","1",ROLLUPX_APPLICATION_CRASH_WORKER));
  ASSERT_TRUE(crash(directory.path,"before_apply","2",ROLLUPX_APPLICATION_CRASH_WORKER));
  ASSERT_TRUE(crash(directory.path,"after_apply","1",ROLLUPX_APPLICATION_CRASH_WORKER));
  Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
  EXPECT_EQ(recovered.apply_committed(),4U); EXPECT_EQ(recovered.ledger_state().accounts[0].balance,70U);
  EXPECT_EQ(recovered.ledger_state().accounts[0].nonce,2U);
}
TEST(RaftSnapshotCrash, KilledStagingAndPublicationRecoverOnlyCompleteAtomicGenerations) {
  const auto image = snapshot_image(); const auto half = image.size()/2;
  for (const auto* operation : {"checkpoint","stage-first","stage-last","publish"}) {
    for (const auto* boundary : {"before","after"}) {
      SCOPED_TRACE(std::string(operation)+"/"+boundary); Directory directory;
      {
        Store initial(directory.path,raft_ledger_manifest(),ledger_genesis());
        if (std::string_view(operation) == "checkpoint") {
          auto state = ledger_snapshot().prefix; state.term = 3; state.voted_for = 2;
          state.commit = 2; initial.persist(state); initial.apply_committed(); initial.create_snapshot();
          state.commit = 4; initial.persist(state); initial.apply_committed();
        } else initial.persist({3,2,{},0});
        if (std::string_view(operation) == "stage-last") initial.stage_snapshot(snapshot_chunk(image,0,half));
        if (std::string_view(operation) == "publish") stage_image(initial,image);
      }
      {
        std::ofstream input(directory.path+"/snapshot-input.bin",std::ios::binary);
        input.write(image.data(),static_cast<std::streamsize>(image.size()));
        ASSERT_TRUE(input.good());
      }
      ASSERT_TRUE(crash(directory.path,boundary,operation,ROLLUPX_SNAPSHOT_CRASH_WORKER));
      const bool after = std::string_view(boundary) == "after";
      for (int reopen = 0; reopen < 2; ++reopen) {
        Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
        EXPECT_EQ(recovered.load().term,3U); EXPECT_EQ(recovered.load().voted_for,2U);
        const bool applied = std::string_view(operation) == "checkpoint" || (std::string_view(operation) == "publish" && after);
        EXPECT_EQ(recovered.load().commit,applied ? 4U : 0U);
        EXPECT_EQ(recovered.ledger_state().applied_index,applied ? 4U : 0U);
        EXPECT_EQ(recovered.ledger_state().accounts[0].balance,applied ? 70U : 100U);
        EXPECT_EQ(recovered.receipt(ledger_transaction().id).has_value(),applied);
        if (applied) {
          for (const auto& [id,receipt] : ledger_snapshot().receipts) {
            ASSERT_TRUE(recovered.receipt(id)); EXPECT_EQ(*recovered.receipt(id),receipt);
          }
        }
        const auto staged = recovered.staged_snapshot();
        if (std::string_view(operation) == "stage-first") {
          EXPECT_EQ(staged.has_value(),after);
          if (staged) { EXPECT_EQ(staged->next_offset,half); }
        } else if (std::string_view(operation) == "stage-last") {
          ASSERT_TRUE(staged); EXPECT_EQ(staged->next_offset,after ? image.size() : half);
        } else if (std::string_view(operation) == "publish") {
          EXPECT_EQ(staged.has_value(),!after);
        }
        if (std::string_view(operation) == "checkpoint") {
          const auto local = recovered.local_snapshot(); ASSERT_TRUE(local);
          EXPECT_EQ(rollupx::raft::decode_snapshot(raft_ledger_manifest(),ledger_genesis(),*local).prefix.commit,after ? 4U : 2U);
        } else if (std::string_view(operation) == "publish") {
          EXPECT_EQ(recovered.local_snapshot().has_value(),after);
        }
      }
      if (std::string_view(operation) != "checkpoint") {
        Store recovered(directory.path,raft_ledger_manifest(),ledger_genesis());
        stage_image(recovered,image); (void)recovered.publish_staged_snapshot();
        EXPECT_EQ(recovered.ledger_state(),ledger_snapshot().ledger); EXPECT_FALSE(recovered.staged_snapshot());
      }
    }
  }
}
} // namespace
