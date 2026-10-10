#include "../test_support.hpp"
#include "../raft_store_fixture.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <gtest/gtest.h>
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
} // namespace
