#include "../test_support.hpp"
#include "../ledger_fixture.hpp"
#include <chrono>
#include <cerrno>
#include <csignal>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace {
using namespace rollupx;
using namespace rollupx::test;
bool killed_at(const std::string& directory, const char* point) {
  const std::string executable = ROLLUPX_CRASH_WORKER;
  char* args[] = {const_cast<char*>(executable.c_str()), const_cast<char*>(directory.c_str()), const_cast<char*>(point), nullptr};
  pid_t pid = -1;
  if (::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, args, environ) != 0) return false;
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    if (result < 0 && errno != EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(pid, SIGKILL);
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return false;
}
TEST(LedgerCrash, KillsAtEveryDurabilityBoundaryAndNeverDoubleApplies) {
  for (const auto* point : {"before_commit", "after_commit", "before_apply", "after_apply"}) {
    SCOPED_TRACE(point); Directory directory;
    { DurableLedger initial(directory.path, ledger_genesis()); }
    ASSERT_TRUE(killed_at(directory.path, point));
    DurableLedger recovered(directory.path, ledger_genesis());
    const bool committed = std::string_view(point) != "before_commit";
    EXPECT_EQ(recovered.snapshot().applied_index, committed ? 1U : 0U);
    EXPECT_EQ(recovered.snapshot().accounts[0].balance, committed ? 90U : 100U);
    const auto transaction = ledger_transaction();
    EXPECT_EQ(recovered.receipt(transaction.id).has_value(), committed);
    const auto receipt = recovered.submit(transaction);
    EXPECT_EQ(receipt.log_index, 1U); EXPECT_EQ(receipt.result, TransferResult::applied);
    EXPECT_EQ(recovered.snapshot().accounts[0].balance, 90U);
    EXPECT_EQ(recovered.snapshot().accounts[0].nonce, 1U);
  }
}
TEST(LedgerCrash, RepeatedCrashesDuringRecoveryStillConverge) {
  Directory directory;
  { DurableLedger initial(directory.path, ledger_genesis()); }
  ASSERT_TRUE(killed_at(directory.path, "after_commit"));
  ASSERT_TRUE(killed_at(directory.path, "before_apply")); // Dies while replaying committed suffix.
  ASSERT_TRUE(killed_at(directory.path, "after_apply"));  // Dies after replay's atomic WriteBatch.
  DurableLedger recovered(directory.path, ledger_genesis());
  EXPECT_EQ(recovered.snapshot().applied_index, 1U); EXPECT_EQ(recovered.snapshot().accounts[0].balance, 90U);
  EXPECT_EQ(recovered.submit(ledger_transaction()).log_index, 1U);
}
} // namespace
