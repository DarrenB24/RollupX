#include "rollupx/ledger_submission.hpp"
#include "../raft_store_fixture.hpp"
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
using namespace std::chrono_literals;
EventResult await(Ticket ticket) {
  if (ticket.admission != Admission::queued || ticket.result.wait_for(3s) != std::future_status::ready)
    throw std::runtime_error("runtime completion deadline");
  return ticket.result.get();
}
class Gate {
 public:
  void enter() { std::unique_lock lock(mutex); entered = true; cv.notify_all();
    if (!cv.wait_for(lock,3s,[&] { return released; })) throw std::runtime_error("application gate deadline"); }
  bool wait() { std::unique_lock lock(mutex); return cv.wait_for(lock,3s,[&] { return entered; }); }
  void release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
 private:
  std::mutex mutex; std::condition_variable cv; bool entered = false, released = false;
};
struct Harness {
  Harness(SubmissionOptions options = {}) {
    runtime = std::make_unique<Runtime>(raft_config(),State{},[&](const State& state) {
      std::lock_guard lock(storage_mutex); disk = state;
    },[&](Index commit) {
      if (block_application && commit > 1) gate.enter();
      std::lock_guard lock(storage_mutex);
      while (ledger.applied_index < commit) {
        const auto plan = plan_committed_entry(ledger_genesis(),ledger,disk,ledger.applied_index+1,
          [&](const Hash& id) { return find(id); });
        for (const auto& receipt : plan.new_receipts) receipts.emplace(receipt.transaction_id,receipt);
        ledger = plan.state;
      }
      return ledger.applied_index;
    },[&](const Envelope& envelope) {
      std::lock_guard lock(output_mutex); outbound.push_back(envelope); output_cv.notify_all(); return true;
    },RuntimeOptions{64,1024*1024,false,10ms},[&] { return time.load(); },
    ledger_proposal_policy(ledger_genesis(),[&] { std::lock_guard lock(storage_mutex); return ledger; },reader()));
    client = std::make_unique<LedgerSubmission>(runtime->handle(),reader(),options);
  }
  ~Harness() { client->close(); gate.release(); if (runtime) runtime->stop(); }
  ReceiptLookup reader() { return [&](const Hash& id) { std::lock_guard lock(storage_mutex); return find(id); }; }
  std::optional<LedgerReceipt> find(const Hash& id) const {
    const auto found = receipts.find(id); return found == receipts.end() ? std::nullopt : std::optional{found->second};
  }
  void elect() {
    time = 600; (void)await(runtime->handle().tick());
    (void)await(runtime->handle().receive({2,1,VoteResponse{1,true}}));
    (void)await(runtime->handle().receive({3,1,VoteResponse{1,true}})); ack();
    if (runtime->status().commit != 1) throw std::runtime_error("leader barrier missing");
  }
  void ack() {
    for (unsigned round = 0; round < 20; ++round) {
      std::vector<Envelope> messages;
      { std::lock_guard lock(output_mutex); messages.swap(outbound); }
      if (messages.empty()) return;
      for (const auto& message : messages) if (const auto* request = std::get_if<AppendRequest>(&message.message))
        (void)await(runtime->handle().receive({message.to,1,AppendResponse{request->term,request->rpc,true,
          request->previous_index+request->entries.size(),0}}));
    }
    throw std::runtime_error("acknowledgments did not quiesce");
  }
  bool proposed() {
    std::unique_lock lock(output_mutex); return output_cv.wait_for(lock,3s,[&] {
      for (const auto& message : outbound) if (const auto* request = std::get_if<AppendRequest>(&message.message))
        for (const auto& entry : request->entries) if (!entry.command.empty()) return true;
      return false;
    });
  }
  std::future<SubmissionResult> submit(SignedTransaction transaction = ledger_transaction()) {
    return std::async(std::launch::async,[&,transaction] { return client->submit(transaction,std::chrono::steady_clock::now()+2s); });
  }
  std::mutex storage_mutex, output_mutex; std::condition_variable output_cv;
  State disk; LedgerState ledger{0,ledger_genesis().accounts}; std::map<Hash,LedgerReceipt> receipts;
  std::vector<Envelope> outbound; std::atomic<Millis> time{0}; std::atomic_bool block_application{false}; Gate gate;
  std::unique_ptr<Runtime> runtime; std::unique_ptr<LedgerSubmission> client;
};
TEST(LedgerAdmission, SignatureNonceBalanceAndChainAreCheckedWithoutMutation) {
  Harness test; test.elect();
  for (const auto& [transaction,reason] : std::vector<std::pair<SignedTransaction,std::string>>{
      {ledger_transaction(1),"wrong_nonce"},{ledger_transaction(0,101),"insufficient_balance"},
      {ledger_transaction(0,0),"zero_amount"},
      {sign_transfer(ledger_key(),Transfer{99,ledger_genesis().accounts[0].address,ledger_genesis().accounts[1].address,1,0}),"wrong_chain"}}) {
    const auto result = test.client->submit(transaction,std::chrono::steady_clock::now()+1s);
    EXPECT_EQ(result.code,SubmissionCode::rejected); EXPECT_EQ(result.reason,reason); EXPECT_FALSE(result.receipt);
  }
  auto malformed = ledger_transaction(); malformed.signature[0] ^= 1;
  EXPECT_EQ(test.client->submit(malformed,std::chrono::steady_clock::now()+1s).code,SubmissionCode::invalid);
  test.runtime->stop(); EXPECT_EQ(test.disk.log.size(),1U); EXPECT_EQ(test.ledger.accounts[0].balance,100U);
  EXPECT_TRUE(test.receipts.empty());
}
TEST(LedgerAdmission, OutstandingTransferBlocksConflictsAndConcurrentDuplicateDoesNotAppendTwice) {
  Harness test; test.elect(); auto first = test.submit(); ASSERT_TRUE(test.proposed());
  auto duplicate = test.submit(); ASSERT_EQ(duplicate.wait_for(1s),std::future_status::ready);
  EXPECT_EQ(duplicate.get().code,SubmissionCode::busy);
  const auto conflict = test.client->submit(ledger_transaction(0,20),std::chrono::steady_clock::now()+1s);
  EXPECT_EQ(conflict.code,SubmissionCode::busy); EXPECT_FALSE(conflict.receipt);
  EXPECT_EQ(first.wait_for(0ms),std::future_status::timeout);
  test.ack(); const auto confirmed = first.get(); ASSERT_EQ(confirmed.code,SubmissionCode::confirmed);
  ASSERT_TRUE(confirmed.receipt); EXPECT_EQ(confirmed.receipt->log_index,2U);
  const auto retry = test.client->submit(ledger_transaction(),std::chrono::steady_clock::now()+1s);
  EXPECT_EQ(retry.code,SubmissionCode::confirmed); EXPECT_EQ(retry.receipt,confirmed.receipt);
  test.runtime->stop(); EXPECT_EQ(test.disk.log.size(),2U); EXPECT_EQ(test.receipts.size(),1U);
  EXPECT_EQ(test.ledger.accounts[0].balance,90U); EXPECT_EQ(test.ledger.accounts[0].nonce,1U);
}
TEST(LedgerConfirmation, MajorityCommitIsNotConfirmationUntilApplicationPublishesReceipt) {
  Harness test; test.elect(); test.block_application = true;
  auto submitted = test.submit(); ASSERT_TRUE(test.proposed());
  auto acknowledgments = std::async(std::launch::async,[&] { test.ack(); }); ASSERT_TRUE(test.gate.wait());
  EXPECT_EQ(submitted.wait_for(0ms),std::future_status::timeout);
  { std::lock_guard lock(test.storage_mutex); EXPECT_EQ(test.disk.commit,2U); EXPECT_EQ(test.ledger.applied_index,1U); }
  test.gate.release(); acknowledgments.get(); const auto result = submitted.get();
  ASSERT_EQ(result.code,SubmissionCode::confirmed); ASSERT_TRUE(result.receipt);
  EXPECT_EQ(result.receipt->result,TransferResult::applied);
}
TEST(LedgerConfirmation, TimeoutAfterLocalAppendCanLaterCommitAndRetryReturnsOriginalReceipt) {
  Harness test({16,50ms,1ms}); test.elect(); auto submitted = test.submit(); ASSERT_TRUE(test.proposed());
  EXPECT_EQ(submitted.get().code,SubmissionCode::deadline);
  EXPECT_FALSE(test.reader()(ledger_transaction().id)); test.ack();
  const auto retry = test.client->submit(ledger_transaction(),std::chrono::steady_clock::now()+1s);
  EXPECT_EQ(retry.code,SubmissionCode::confirmed); ASSERT_TRUE(retry.receipt); EXPECT_EQ(retry.receipt->log_index,2U);
}
TEST(LedgerConfirmation, LeadershipLossReturnsAmbiguousFailureAndFollowerCanReturnExistingReceipt) {
  Harness test; test.elect(); auto submitted = test.submit(); ASSERT_TRUE(test.proposed());
  (void)await(test.runtime->handle().receive({2,1,VoteRequest{2,2,1}}));
  EXPECT_EQ(submitted.get().code,SubmissionCode::unavailable);
  const auto command = encode_transfer_command({ledger_transaction()});
  (void)await(test.runtime->handle().receive({2,1,AppendRequest{2,1,1,1,{{2,command}},2}}));
  const auto retry = test.client->submit(ledger_transaction(),std::chrono::steady_clock::now()+1s);
  EXPECT_EQ(retry.code,SubmissionCode::confirmed); ASSERT_TRUE(retry.receipt); EXPECT_EQ(retry.receipt->log_index,2U);
  EXPECT_EQ(test.client->submit(ledger_transaction(1),std::chrono::steady_clock::now()+1s).code,SubmissionCode::unavailable);
}
TEST(LedgerConfirmation, InvalidSignatureCannotUseCachedTransactionId) {
  Harness test; test.elect(); auto submitted = test.submit(); ASSERT_TRUE(test.proposed()); test.ack();
  ASSERT_EQ(submitted.get().code,SubmissionCode::confirmed);
  auto bad = ledger_transaction(); bad.signature[0] ^= 1;
  EXPECT_EQ(test.client->submit(bad,std::chrono::steady_clock::now()+1s).code,SubmissionCode::invalid);
}
TEST(LedgerConfirmation, CommittedRejectionReturnsOriginalReceiptAndBypassesLeaderAdmission) {
  Harness test; const auto history = raft_ledger_history();
  (void)await(test.runtime->handle().receive({2,1,AppendRequest{1,1,0,0,history.log,history.commit}}));
  const auto rejected = ledger_transaction(1,1000);
  const auto result = test.client->submit(rejected,std::chrono::steady_clock::now()+1s);
  EXPECT_EQ(result.code,SubmissionCode::confirmed); ASSERT_TRUE(result.receipt);
  EXPECT_EQ(result.receipt->result,TransferResult::insufficient_balance); EXPECT_EQ(result.receipt->log_index,4U);
  EXPECT_EQ(result.receipt->transaction_offset,1U);
}
TEST(LedgerConfirmation, TimedOutQueuedProposalIsCancelledBeforeOwnerAppendsIt) {
  Harness test; test.elect(); test.block_application = true;
  auto first = test.submit(); ASSERT_TRUE(test.proposed());
  auto acknowledgments = std::async(std::launch::async,[&] { test.ack(); }); ASSERT_TRUE(test.gate.wait());
  const auto queued = test.client->submit(ledger_transaction(1),std::chrono::steady_clock::now()+30ms);
  EXPECT_EQ(queued.code,SubmissionCode::deadline); EXPECT_FALSE(queued.receipt);
  test.gate.release(); acknowledgments.get(); EXPECT_EQ(first.get().code,SubmissionCode::confirmed);
  (void)await(test.runtime->handle().tick()); test.runtime->stop();
  EXPECT_EQ(test.disk.log.size(),2U); EXPECT_EQ(test.ledger.accounts[0].nonce,1U);
}
TEST(LedgerConfirmation, CallBoundCloseCancellationAndExpiredRuntimeAreExplicit) {
  Harness test({1,2s,1ms}); test.elect(); auto submitted = test.submit(); ASSERT_TRUE(test.proposed());
  EXPECT_EQ(test.client->submit(ledger_transaction(),std::chrono::steady_clock::now()+1s).code,SubmissionCode::busy);
  EXPECT_EQ(test.client->status().backpressure,1U); test.client->close();
  EXPECT_EQ(submitted.get().code,SubmissionCode::unavailable); EXPECT_EQ(test.client->status().active,0U);
  LedgerSubmission client(test.runtime->handle(),test.reader());
  EXPECT_EQ(client.submit(ledger_transaction(),std::chrono::steady_clock::now()+1s,[] { return true; }).code,SubmissionCode::cancelled);
  EXPECT_EQ(client.submit(ledger_transaction(),std::chrono::steady_clock::now()).code,SubmissionCode::deadline);
  test.runtime.reset(); EXPECT_EQ(client.submit(ledger_transaction(),std::chrono::steady_clock::now()+1s).code,SubmissionCode::unavailable);
}
TEST(LedgerConfirmation, ReceiptReadFailuresAndWrongIdentitiesFailClosed) {
  Harness test;
  for (bool corrupt : {false,true}) {
    LedgerSubmission client(test.runtime->handle(),[&](const Hash&) -> std::optional<LedgerReceipt> {
      if (!corrupt) throw std::runtime_error("disk read failed");
      return LedgerReceipt{ledger_transaction(0,20).id,1,0,TransferResult::applied};
    });
    EXPECT_EQ(client.submit(ledger_transaction(),std::chrono::steady_clock::now()+1s).code,SubmissionCode::failed);
    EXPECT_TRUE(client.status().failed); EXPECT_TRUE(client.failure()); EXPECT_EQ(client.status().active,0U);
    EXPECT_EQ(client.submit(ledger_transaction(),std::chrono::steady_clock::now()+1s).code,SubmissionCode::failed);
  }
}
TEST(LedgerAdmission, PolicyErrorsCannotMasqueradeAsTransactionRejection) {
  const auto history = raft_ledger_history();
  auto policy = ledger_proposal_policy(ledger_genesis(),[] { return LedgerState{0,ledger_genesis().accounts}; },
    [](const Hash&) -> std::optional<LedgerReceipt> { return {}; });
  EXPECT_THROW(policy(history,encode_transfer_command({ledger_transaction()})),std::runtime_error);
  policy = ledger_proposal_policy(ledger_genesis(),[]() -> LedgerState { throw std::invalid_argument("corrupt ledger"); },
    [](const Hash&) -> std::optional<LedgerReceipt> { return {}; });
  EXPECT_THROW(policy(history,encode_transfer_command({ledger_transaction()})),std::invalid_argument);
}
TEST(LedgerAdmission, SingleTransferLimitMalformedCommandsAndLogCapacityRejectWithoutMutation) {
  State state{1,{},{{1,{}}},1}; LedgerState applied{1,ledger_genesis().accounts};
  auto policy = ledger_proposal_policy(ledger_genesis(),[&] { return applied; },[](const Hash&) -> std::optional<LedgerReceipt> { return {}; });
  EXPECT_EQ(policy(state,"bad"),"malformed_command");
  EXPECT_EQ(policy(state,encode_transfer_command({ledger_transaction(),ledger_transaction(1)})),"single_transfer_required");
  state.log.resize(kMaxLogEntries,Entry{1,{}}); state.commit = kMaxLogEntries; applied.applied_index = state.commit;
  EXPECT_EQ(policy(state,encode_transfer_command({ledger_transaction()})),"log_full");
}
} // namespace
