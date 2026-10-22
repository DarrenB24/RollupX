#include "rollupx/raft_runtime.hpp"
#include "../raft_store_fixture.hpp"
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
using namespace std::chrono_literals;
RuntimeOptions manual(std::size_t count = 32, std::size_t bytes = 1024*1024) { return {count,bytes,false,10ms}; }
EventResult await(Ticket ticket) {
  if (ticket.admission != Admission::queued || ticket.result.wait_for(3s) != std::future_status::ready)
    throw std::runtime_error("runtime completion deadline missed");
  return ticket.result.get();
}
// A failed test cannot leave the owner's persistence callback blocked forever.
class Gate {
 public:
  void enter() {
    std::unique_lock lock(mutex); entered = true; condition.notify_all();
    if (!condition.wait_for(lock,3s,[&] { return released; })) throw std::runtime_error("test gate deadline missed");
  }
  bool wait() { std::unique_lock lock(mutex); return condition.wait_for(lock,3s,[&] { return entered; }); }
  void release() { std::lock_guard lock(mutex); released = true; condition.notify_all(); }
  ~Gate() { release(); }
 private:
  std::mutex mutex; std::condition_variable condition; bool entered = false, released = false;
};
struct Harness {
  State disk;
  std::atomic<Millis> time{0};
  std::vector<Envelope> sent; // Owner writes; inspect only after a completion/stop.
  std::vector<std::thread::id> owners;
  Index applied = 0;
  std::unique_ptr<Runtime> runtime;
  void start(RuntimeOptions options = manual(), ProposalCheck check = {}) {
    runtime = std::make_unique<Runtime>(raft_config(),disk,[&](const State& state) {
      owners.push_back(std::this_thread::get_id()); disk = state;
    },[&](Index commit) { owners.push_back(std::this_thread::get_id()); applied = commit; return applied; },
    [&](const Envelope& message) { owners.push_back(std::this_thread::get_id()); sent.push_back(message); return true; },
    options,[&] { return time.load(); },std::move(check));
  }
  Runtime::Handle handle() { return runtime->handle(); }
  void elect() {
    time = 600; EXPECT_EQ(await(handle().tick()).status.role,Role::candidate);
    (void)await(handle().receive({2,1,VoteResponse{1,true}}));
    (void)await(handle().receive({3,1,VoteResponse{1,true}}));
    const auto requests = sent; // The owner is idle in manual mode after completion.
    for (const auto& message : requests) if (const auto* request = std::get_if<AppendRequest>(&message.message)) {
      (void)await(handle().receive({message.to,1,AppendResponse{1,request->rpc,true,1,0}}));
    }
    ASSERT_EQ(runtime->status().commit,1U);
  }
};
TEST(RaftRuntime, PersistenceApplicationAndDispatchHaveExactlyOneOwner) {
  Harness test; test.start(); test.elect();
  const auto result = await(test.handle().propose("command"));
  EXPECT_EQ(result.completion,Completion::processed); EXPECT_EQ(result.proposed_index,2U);
  EXPECT_EQ(result.status.commit,1U); EXPECT_EQ(result.status.applied,1U); // Not client confirmation.
  test.runtime->stop(); ASSERT_FALSE(test.owners.empty());
  for (const auto owner : test.owners) {
    EXPECT_EQ(owner,test.owners.front()); EXPECT_NE(owner,std::this_thread::get_id());
  }
}
TEST(RaftRuntime, UnaryReplyWaitsForPersistenceAndApplication) {
  Gate gate; State disk; std::atomic<Index> applied{0};
  Runtime runtime(raft_config(),{},[&](const State& state) { disk = state; },[&](Index commit) {
    if (commit == 1) gate.enter(); applied = commit; return commit;
  },[](const Envelope&) { return true; },manual());
  auto ticket = runtime.handle().receive({2,1,AppendRequest{1,1,0,0,{{1,"command"}},1}});
  ASSERT_TRUE(gate.wait()); EXPECT_EQ(ticket.result.wait_for(0ms),std::future_status::timeout);
  EXPECT_EQ(applied.load(),0U); gate.release(); const auto result = await(std::move(ticket));
  ASSERT_TRUE(result.reply); EXPECT_TRUE(std::get<AppendResponse>(result.reply->message).success);
  EXPECT_EQ(result.status.commit,1U); EXPECT_EQ(result.status.applied,1U);
  runtime.stop(); EXPECT_EQ(disk.commit,1U);
}
TEST(RaftRuntime, ActiveEventCountsAgainstCapacityAndQueuedCancellationHasNoEffect) {
  Gate gate; State disk;
  Runtime runtime(raft_config(),{},[&](const State& state) { if (state.term == 1 && !state.voted_for) gate.enter(); disk = state; },
    [](Index commit) { return commit; },[](const Envelope&) { return true; },manual(2));
  auto active = runtime.handle().receive({2,1,VoteRequest{1,0,0}}); ASSERT_TRUE(gate.wait());
  auto pending = runtime.handle().receive({3,1,VoteRequest{2,0,0}}); ASSERT_EQ(pending.admission,Admission::queued);
  EXPECT_EQ(runtime.handle().tick().admission,Admission::full); EXPECT_EQ(runtime.status().pending_events,2U);
  pending.cancel(); gate.release(); EXPECT_EQ(await(std::move(active)).completion,Completion::processed);
  EXPECT_EQ(await(std::move(pending)).completion,Completion::cancelled);
  runtime.stop(); EXPECT_EQ(disk.term,1U); EXPECT_EQ(disk.voted_for,2U);
  EXPECT_EQ(runtime.status().pending_bytes,0U);
}
TEST(RaftRuntime, PayloadBudgetIncludesRetainedStringAndVectorCapacity) {
  Harness test; test.start(manual(4,1024));
  std::string reserved = "x"; reserved.reserve(4096);
  EXPECT_EQ(test.handle().propose(std::move(reserved)).admission,Admission::invalid);
  AppendRequest append{1,1,0,0,{},0}; append.entries.reserve(100);
  EXPECT_EQ(test.handle().receive({2,1,std::move(append)}).admission,Admission::invalid);
  EXPECT_EQ(test.handle().propose("").admission,Admission::invalid);
  EXPECT_EQ(test.runtime->status().pending_bytes,0U); EXPECT_EQ(test.runtime->status().admission_rejected,3U);
}
TEST(RaftRuntime, ByteBudgetRejectsWorkBeforeEventCountLimit) {
  Gate gate;
  Runtime runtime(raft_config(),{},[&](const State&) { gate.enter(); },[](Index commit) { return commit; },
    [](const Envelope&) { return true; },manual(8,2048));
  auto active = runtime.handle().receive({2,1,VoteRequest{1,0,0}}); ASSERT_TRUE(gate.wait());
  auto queued = runtime.handle().propose(std::string(1000,'x')); ASSERT_EQ(queued.admission,Admission::queued);
  EXPECT_EQ(runtime.handle().propose(std::string(1000,'x')).admission,Admission::full);
  EXPECT_EQ(runtime.status().pending_events,2U); EXPECT_LE(runtime.status().pending_bytes,2048U);
  gate.release(); (void)await(std::move(active)); EXPECT_EQ(await(std::move(queued)).completion,Completion::rejected);
}
TEST(RaftRuntime, GracefulStopCompletesActiveWorkAndCancelsEveryQueuedRequest) {
  Gate gate; State disk;
  Runtime runtime(raft_config(),{},[&](const State& state) { gate.enter(); disk = state; },
    [](Index commit) { return commit; },[](const Envelope&) { return true; },manual());
  const auto handle = runtime.handle(); auto active = handle.receive({2,1,VoteRequest{1,0,0}});
  ASSERT_TRUE(gate.wait()); auto queued = handle.receive({3,1,VoteRequest{2,0,0}});
  runtime.request_stop(); EXPECT_EQ(handle.tick().admission,Admission::stopped);
  gate.release(); EXPECT_EQ(await(std::move(active)).completion,Completion::processed);
  EXPECT_EQ(await(std::move(queued)).completion,Completion::stopped);
  runtime.stop(); runtime.stop(); EXPECT_EQ(disk.term,1U); EXPECT_EQ(runtime.status().pending_events,0U);
}
TEST(RaftRuntime, LateCallbackHandleCannotAccessDestroyedOrReplacementRuntime) {
  Runtime::Handle old;
  { Harness test; test.start(); old = test.handle(); }
  EXPECT_EQ(old.tick().admission,Admission::stopped);
  Harness replacement; replacement.start(); EXPECT_EQ(old.propose("late").admission,Admission::stopped);
  EXPECT_EQ(await(replacement.handle().tick()).completion,Completion::processed);
}
TEST(RaftRuntime, ConcurrentStopCallsJoinOnceAndLeaveNoPendingWork) {
  Harness test; test.start(); std::vector<std::future<void>> callers;
  for (int i = 0; i < 8; ++i) callers.push_back(std::async(std::launch::async,[&] { test.runtime->stop(); }));
  for (auto& caller : callers) EXPECT_NO_THROW(caller.get());
  EXPECT_TRUE(test.runtime->status().stopped); EXPECT_EQ(test.runtime->status().pending_events,0U);
}
TEST(RaftRuntime, AmbiguousPersistenceFailureStopsOwnerAndEmitsNoReply) {
  Gate gate;
  Runtime runtime(raft_config(),{},[&](const State&) { gate.enter(); throw ProposalRejected("ambiguous storage failure"); },
    [](Index commit) { return commit; },[](const Envelope&) { return true; },manual());
  auto active = runtime.handle().receive({2,1,VoteRequest{1,0,0}}); ASSERT_TRUE(gate.wait());
  auto queued = runtime.handle().tick(); gate.release();
  const auto result = await(std::move(active)); EXPECT_EQ(result.completion,Completion::failed); EXPECT_FALSE(result.reply);
  EXPECT_EQ(await(std::move(queued)).completion,Completion::stopped);
  EXPECT_TRUE(runtime.status().failed); ASSERT_TRUE(runtime.failure());
  EXPECT_THROW(std::rethrow_exception(runtime.failure()),ProposalRejected);
  EXPECT_EQ(runtime.handle().tick().admission,Admission::stopped);
}
TEST(RaftRuntime, ApplicationFailureSuppressesAcknowledgmentAfterCommit) {
  State disk;
  Runtime runtime(raft_config(),{},[&](const State& state) { disk = state; },
    [](Index commit) -> Index { if (commit) throw std::runtime_error("apply failed"); return 0; },
    [](const Envelope&) { return true; },manual());
  const auto result = await(runtime.handle().receive({2,1,AppendRequest{1,1,0,0,{{1,"command"}},1}}));
  EXPECT_EQ(result.completion,Completion::failed); EXPECT_FALSE(result.reply);
  EXPECT_EQ(result.status.commit,1U); EXPECT_EQ(result.status.applied,0U); EXPECT_TRUE(result.status.failed);
  runtime.stop(); EXPECT_EQ(disk.commit,1U);
}
TEST(RaftRuntime, StartupRequiresApplicationToMatchRecoveredCommit) {
  const State recovered{1,{},{{1,"command"}},1};
  EXPECT_THROW((Runtime(raft_config(),recovered,[](const State&) {},[](Index) { return 0; },
    [](const Envelope&) { return true; },manual())),std::runtime_error);
}
TEST(RaftRuntime, NotLeaderIsRecoverableButBackwardClockStopsRuntime) {
  Harness test; test.start();
  EXPECT_EQ(await(test.handle().propose("not-leader")).completion,Completion::rejected);
  EXPECT_FALSE(test.runtime->status().failed); test.time = 50; (void)await(test.handle().tick());
  test.time = 49; EXPECT_EQ(await(test.handle().tick()).completion,Completion::failed);
  EXPECT_TRUE(test.runtime->status().failed);
}
TEST(RaftRuntime, TransportBackpressureIsCountedAndDispatchExceptionsFailClosed) {
  for (const bool throwing : {false,true}) {
    std::atomic<Millis> time{0};
    Runtime runtime(raft_config(),{},[](const State&) {},[](Index commit) { return commit; },
      [throwing](const Envelope&) { if (throwing) throw std::runtime_error("driver failure"); return false; },
      manual(),[&] { return time.load(); });
    time = 600; const auto result = await(runtime.handle().tick());
    EXPECT_EQ(result.completion,throwing ? Completion::failed : Completion::processed);
    EXPECT_EQ(runtime.status().outbound_dropped,throwing ? 0U : 4U); EXPECT_EQ(runtime.status().failed,throwing);
  }
}
TEST(RaftRuntime, ConcurrentProducersCompleteWithoutRacingCoreOrLosingAcceptedEvents) {
  Harness test; test.start(manual(256)); std::atomic<unsigned> completed{0};
  std::vector<std::future<void>> producers;
  for (int producer = 0; producer < 8; ++producer) producers.push_back(std::async(std::launch::async,[&] {
    for (int i = 0; i < 40; ++i) {
      const auto result = await(test.handle().receive({2,1,VoteRequest{1,0,0}}));
      if (result.completion == Completion::processed && result.reply) ++completed;
    }
  }));
  for (auto& producer : producers) EXPECT_NO_THROW(producer.get());
  test.runtime->stop();
  EXPECT_EQ(completed.load(),320U); EXPECT_EQ(test.disk.voted_for,2U); EXPECT_EQ(test.disk.term,1U);
  EXPECT_EQ(test.runtime->status().pending_events,0U);
}
TEST(RaftRuntime, AutomaticSchedulerDrivesElectionWithoutCallerTicks) {
  std::mutex mutex; std::condition_variable condition; bool requested = false;
  auto config = raft_config(); config.heartbeat = 5; config.election_min = 10; config.election_max = 20;
  Runtime runtime(config,{},[](const State&) {},[](Index commit) { return commit; },[&](const Envelope&) {
    std::lock_guard lock(mutex); requested = true; condition.notify_all(); return true;
  });
  std::unique_lock lock(mutex); EXPECT_TRUE(condition.wait_for(lock,3s,[&] { return requested; }));
  lock.unlock(); runtime.stop(); EXPECT_GT(runtime.status().elections,0U);
}
TEST(RaftRuntime, OwnerCanRequestShutdownButCannotJoinItself) {
  for (const bool join : {false,true}) {
    Runtime* current = nullptr; std::atomic<Millis> time{0};
    Runtime runtime(raft_config(),{},[](const State&) {},[](Index commit) { return commit; },[&](const Envelope&) {
      if (join) current->stop(); else current->request_stop(); return true;
    },manual(),[&] { return time.load(); });
    current = &runtime; time = 600;
    EXPECT_EQ(await(runtime.handle().tick()).completion,join ? Completion::failed : Completion::processed);
    runtime.stop(); EXPECT_TRUE(runtime.status().stopped); EXPECT_EQ(runtime.status().failed,join);
  }
}
TEST(RaftRuntime, SharedApplicationReplaysCommittedTransfersAndKeepsRetriesIdempotent) {
  auto disk = raft_ledger_history(); LedgerState ledger{0,ledger_genesis().accounts};
  std::map<Hash,LedgerReceipt> receipts;
  const ReceiptLookup lookup = [&](const Hash& id) -> std::optional<LedgerReceipt> {
    const auto it = receipts.find(id); return it == receipts.end() ? std::nullopt : std::optional{it->second};
  };
  Runtime runtime(raft_config(),disk,[&](const State& state) { disk = state; },[&](Index commit) {
    while (ledger.applied_index < commit) {
      const auto plan = plan_committed_entry(ledger_genesis(),ledger,disk,ledger.applied_index+1,lookup);
      for (const auto& receipt : plan.new_receipts) receipts.emplace(receipt.transaction_id,receipt);
      ledger = plan.state;
    }
    audit_raft_application(ledger_genesis(),ledger,disk,lookup); return ledger.applied_index;
  },[](const Envelope&) { return true; },manual());
  EXPECT_EQ(runtime.status().applied,4U);
  auto retry = AppendRequest{1,1,4,1,{{1,encode_transfer_command({ledger_transaction()})}},5};
  const auto result = await(runtime.handle().receive({2,1,std::move(retry)}));
  EXPECT_EQ(result.status.applied,5U); runtime.stop();
  EXPECT_EQ(ledger.accounts[0].balance,70U); EXPECT_EQ(ledger.accounts[0].nonce,2U);
  EXPECT_EQ(receipts.size(),3U); EXPECT_EQ(receipts.at(ledger_transaction().id).log_index,2U);
}
TEST(RaftRuntime, OwnerPolicyRejectsWithoutPersistenceAndDoesNotRunOnFollowers) {
  unsigned checks = 0; std::thread::id checked_on;
  Harness test; test.start(manual(),[&](const State& state, std::string_view command) -> std::optional<std::string> {
    ++checks; checked_on = std::this_thread::get_id();
    EXPECT_EQ(state.commit,1U); EXPECT_EQ(command,"command"); return "busy";
  });
  EXPECT_EQ(await(test.handle().propose("command")).completion,Completion::rejected); EXPECT_EQ(checks,0U);
  test.elect(); const auto before = test.disk;
  const auto result = await(test.handle().propose("command"));
  EXPECT_EQ(result.completion,Completion::rejected); EXPECT_EQ(result.policy_rejection,"busy");
  EXPECT_FALSE(result.proposed_index); EXPECT_EQ(test.disk,before); EXPECT_EQ(checks,1U);
  EXPECT_NE(checked_on,std::this_thread::get_id()); EXPECT_FALSE(test.runtime->status().failed);
}
TEST(RaftRuntime, PolicyExceptionsIncludingProposalRejectedAreFatal) {
  for (bool explicit_rejection : {false,true}) {
    Harness test; test.start(manual(),[&](const State&, std::string_view) -> std::optional<std::string> {
      if (explicit_rejection) throw ProposalRejected("storage callback failed");
      throw std::runtime_error("storage callback failed");
    }); test.elect();
    EXPECT_EQ(await(test.handle().propose("command")).completion,Completion::failed);
    EXPECT_TRUE(test.runtime->status().failed); EXPECT_EQ(test.disk.log.size(),1U);
    EXPECT_TRUE(test.runtime->failure());
  }
}
TEST(RaftRuntime, ExpiredHandleStatusIsStoppedAndCannotObserveReplacementRuntime) {
  Runtime::Handle handle;
  { Harness test; test.start(); handle = test.handle(); EXPECT_FALSE(handle.status().stopped); }
  Harness replacement; replacement.start();
  EXPECT_TRUE(handle.status().stopped); EXPECT_FALSE(replacement.handle().status().stopped);
}
} // namespace
