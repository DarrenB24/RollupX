#include "rollupx/raft_transport.hpp"
#include "rollupx/ledger_submission.hpp"
#include "../raft_store_fixture.hpp"
#include <condition_variable>
#include <algorithm>
#include <gtest/gtest.h>
#include <mutex>
#include <type_traits>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
using namespace std::chrono_literals;
WireCodec codec(NodeId local) { return WireCodec("transport",{"n1","n2","n3","n4","n5"},raft_ledger_manifest().genesis_digest,local); }
Envelope vote(NodeId to = 2) { return {1,to,VoteRequest{1,0,0}}; }
RuntimeOptions manual() { return {64,1024*1024,false,10ms}; }
EventResult await(Ticket ticket) {
  if (ticket.admission != Admission::queued || ticket.result.wait_for(3s) != std::future_status::ready)
    throw std::runtime_error("runtime deadline missed");
  return ticket.result.get();
}
RpcResult reply(NodeId peer, const WireRequest& wire) {
  const auto call = codec(peer).decode_request(wire); const auto& request = std::get<VoteRequest>(call.message);
  return {RpcCode::ok,codec(peer).encode_response({peer,1,VoteResponse{request.term,true}},call)};
}
class Gate {
 public:
  void enter() { std::unique_lock lock(mutex); ++entered; cv.notify_all();
    if (!cv.wait_for(lock,3s,[&] { return open; })) throw std::runtime_error("gate deadline"); }
  bool wait(unsigned count = 1) { std::unique_lock lock(mutex); return cv.wait_for(lock,3s,[&] { return entered >= count; }); }
  void release() { std::lock_guard lock(mutex); open = true; cv.notify_all(); }
 private:
  std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool open = false;
};
struct Node {
  explicit Node(NodeId id = 1) {
    auto config = raft_config(); config.id = id;
    runtime = std::make_unique<Runtime>(config,State{},[&](const State& state) { disk = state; },
      [](Index commit) { return commit; },[](const Envelope&) { return true; },manual());
  }
  State disk; std::unique_ptr<Runtime> runtime;
};
TEST(RaftTransport, SuccessfulCorrelatedResponseReachesOwnerAndCannotRebind) {
  Node node; PeerDispatcher transport(codec(1),[](NodeId peer, const WireRequest& request, RpcDeadline, std::stop_token) { return reply(peer,request); });
  EXPECT_FALSE(transport.try_enqueue(vote())); transport.bind(node.runtime->handle());
  EXPECT_THROW(transport.bind(node.runtime->handle()),std::logic_error);
  ASSERT_TRUE(transport.try_enqueue(vote())); ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s));
  EXPECT_EQ(await(node.runtime->handle().tick()).status.term,1U); EXPECT_EQ(transport.status().delivered,1U);
  transport.stop(); EXPECT_FALSE(transport.try_enqueue(vote()));
}
TEST(RaftTransport, OneActiveAndOnePendingPerPeerAndOtherPeersProgressIndependently) {
  Node node; Gate gate;
  PeerDispatcher transport(codec(1),[&](NodeId peer, const WireRequest& request, RpcDeadline, std::stop_token) {
    gate.enter(); return reply(peer,request);
  },2s); transport.bind(node.runtime->handle());
  ASSERT_TRUE(transport.try_enqueue(vote())); ASSERT_TRUE(gate.wait());
  EXPECT_TRUE(transport.try_enqueue(vote())); EXPECT_FALSE(transport.try_enqueue(vote()));
  ASSERT_TRUE(transport.try_enqueue(vote(3))); ASSERT_TRUE(gate.wait(2));
  EXPECT_EQ(transport.status().active,2U); EXPECT_EQ(transport.status().queued,1U);
  gate.release(); ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s));
  EXPECT_EQ(transport.status().delivered,3U); EXPECT_EQ(transport.status().backpressure,1U);
}
TEST(RaftTransport, RpcFailureMissingResponseAndWrongPeerNeverReachCore) {
  for (int mode = 0; mode < 3; ++mode) {
    Node node;
    PeerDispatcher transport(codec(1),[&](NodeId peer, const WireRequest& request, RpcDeadline, std::stop_token) -> RpcResult {
      if (mode == 0) return {RpcCode::unavailable};
      if (mode == 1) return {RpcCode::ok};
      auto result = reply(peer,request); std::get<internal::v1::RequestVoteResponse>(*result.response).mutable_context()->set_sender_id("n3");
      return result;
    }); transport.bind(node.runtime->handle()); ASSERT_TRUE(transport.try_enqueue(vote()));
    ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s));
    EXPECT_EQ(await(node.runtime->handle().tick()).status.term,0U); EXPECT_EQ(transport.status().delivered,0U);
    EXPECT_EQ(transport.status().rpc_errors,mode == 0 ? 1U : 0U);
    EXPECT_EQ(transport.status().invalid_replies,mode == 0 ? 0U : 1U);
  }
}
TEST(RaftTransport, LateSuccessfulBackendResponseIsDiscarded) {
  Node node;
  PeerDispatcher transport(codec(1),[](NodeId peer, const WireRequest& request, RpcDeadline deadline, std::stop_token) {
    std::mutex mutex; std::condition_variable cv; std::unique_lock lock(mutex);
    cv.wait_until(lock,deadline,[] { return false; }); return reply(peer,request);
  },10ms); transport.bind(node.runtime->handle()); ASSERT_TRUE(transport.try_enqueue(vote()));
  ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s));
  EXPECT_EQ(transport.status().rpc_errors,1U); EXPECT_EQ(await(node.runtime->handle().tick()).status.term,0U);
}
TEST(RaftTransport, StopCancelsActiveCallDiscardsPendingAndPreventsLateHandoff) {
  Node node; std::promise<void> entered; auto started = entered.get_future();
  PeerDispatcher* current = nullptr;
  PeerDispatcher transport(codec(1),[&](NodeId peer, const WireRequest& request, RpcDeadline, std::stop_token token) {
    std::mutex mutex; std::condition_variable cv;
    std::stop_callback callback(token,[&] { (void)current->status(); std::lock_guard lock(mutex); cv.notify_all(); });
    std::unique_lock lock(mutex); entered.set_value();
    if (!cv.wait_for(lock,3s,[&] { return token.stop_requested(); })) throw std::runtime_error("stop not delivered");
    return reply(peer,request); // Even a backend that reports success after cancellation is ignored.
  },2s); current = &transport; transport.bind(node.runtime->handle());
  ASSERT_TRUE(transport.try_enqueue(vote())); ASSERT_EQ(started.wait_for(3s),std::future_status::ready);
  ASSERT_TRUE(transport.try_enqueue(vote())); transport.stop(); transport.stop();
  EXPECT_EQ(transport.status().queued,0U); EXPECT_EQ(transport.status().active,0U);
  EXPECT_EQ(transport.status().delivered,0U); EXPECT_EQ(await(node.runtime->handle().tick()).status.term,0U);
}
TEST(RaftTransport, BackendExceptionStopsAdmissionAndPreservesDiagnostic) {
  Node node; PeerDispatcher transport(codec(1),[](NodeId, const WireRequest&, RpcDeadline, std::stop_token) -> RpcResult {
    throw std::runtime_error("backend failure");
  }); transport.bind(node.runtime->handle()); ASSERT_TRUE(transport.try_enqueue(vote()));
  ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s));
  EXPECT_TRUE(transport.status().failed); ASSERT_TRUE(transport.failure());
  EXPECT_THROW(transport.try_enqueue(vote()),std::runtime_error); EXPECT_EQ(transport.status().delivered,0U);
}
TEST(RaftTransport, ExpiredRuntimeHandleRejectsResponseWithoutDanglingAccess) {
  Runtime::Handle expired; { Node node; expired = node.runtime->handle(); }
  PeerDispatcher transport(codec(1),[](NodeId peer, const WireRequest& request, RpcDeadline, std::stop_token) { return reply(peer,request); });
  transport.bind(expired); ASSERT_TRUE(transport.try_enqueue(vote()));
  ASSERT_TRUE(transport.wait_idle(std::chrono::steady_clock::now()+3s)); EXPECT_EQ(transport.status().runtime_rejected,1U);
}
TEST(RaftIngress, ValidRequestAppliesAndMalformedOrExpiredRequestsHaveNoEffect) {
  Node node(2); RaftIngress ingress(codec(2),node.runtime->handle());
  const auto wire = codec(1).encode_request(vote());
  const auto result = ingress.respond(wire,std::chrono::steady_clock::now()+1s);
  ASSERT_EQ(result.code,RpcCode::ok); ASSERT_TRUE(result.response);
  EXPECT_TRUE(std::get<VoteResponse>(codec(1).decode_response(*result.response,vote()).message).granted);
  auto bad = std::get<internal::v1::RequestVoteRequest>(wire); bad.set_term(99);
  EXPECT_EQ(ingress.respond(bad,std::chrono::steady_clock::now()+1s).code,RpcCode::invalid);
  EXPECT_EQ(ingress.respond(wire,std::chrono::steady_clock::now()).code,RpcCode::deadline);
  EXPECT_EQ(ingress.respond(wire,std::chrono::steady_clock::now()+1s,[] { return true; }).code,RpcCode::cancelled);
  EXPECT_EQ(await(node.runtime->handle().tick()).status.term,1U); EXPECT_EQ(ingress.status().active,0U);
}
TEST(RaftIngress, ActiveCallLimitAndCloseDoNotAcknowledgeAnUnfinishedApplication) {
  Gate gate; auto config = raft_config(); config.id = 2;
  Runtime runtime(config,{},[](const State&) {},[&](Index commit) {
    if (commit) {
      gate.enter();
    }
    return commit;
  },
    [](const Envelope&) { return true; },manual());
  RaftIngress ingress(codec(2),runtime.handle(),{1,2s,1ms});
  const auto request = codec(1).encode_request({1,2,AppendRequest{1,1,0,0,{{1,"command"}},1}});
  auto call = std::async(std::launch::async,[&] { return ingress.respond(request,std::chrono::steady_clock::now()+2s); });
  ASSERT_TRUE(gate.wait()); EXPECT_EQ(ingress.respond(request,std::chrono::steady_clock::now()+1s).code,RpcCode::exhausted);
  ingress.close(); ASSERT_EQ(call.wait_for(1s),std::future_status::ready); EXPECT_EQ(call.get().code,RpcCode::cancelled);
  EXPECT_EQ(ingress.respond(request,std::chrono::steady_clock::now()+1s).code,RpcCode::unavailable);
  gate.release(); EXPECT_EQ(await(runtime.handle().tick()).status.applied,1U);
}
TEST(RaftIngress, DeadlineCancelsQueuedRequestBeforeConsensusMutation) {
  Gate gate; auto config = raft_config(); config.id = 2;
  Runtime runtime(config,{},[&](const State&) { gate.enter(); },[](Index commit) { return commit; },
    [](const Envelope&) { return true; },manual());
  auto active = runtime.handle().receive({1,2,VoteRequest{1,0,0}}); ASSERT_TRUE(gate.wait());
  RaftIngress ingress(codec(2),runtime.handle(),{2,10ms,1ms});
  const auto request = codec(1).encode_request({1,2,VoteRequest{2,0,0}});
  EXPECT_EQ(ingress.respond(request,std::chrono::steady_clock::now()+1s).code,RpcCode::deadline);
  gate.release(); (void)await(std::move(active)); EXPECT_EQ(await(runtime.handle().tick()).status.term,1U);
}
TEST(RaftIngress, StorageFailureAndStoppedRuntimeNeverReturnOk) {
  auto config = raft_config(); config.id = 2;
  Runtime runtime(config,{},[](const State&) { throw std::runtime_error("disk failed"); },[](Index commit) { return commit; },
    [](const Envelope&) { return true; },manual());
  RaftIngress ingress(codec(2),runtime.handle()); const auto request = codec(1).encode_request(vote());
  EXPECT_EQ(ingress.respond(request,std::chrono::steady_clock::now()+1s).code,RpcCode::failed);
  EXPECT_EQ(ingress.respond(request,std::chrono::steady_clock::now()+1s).code,RpcCode::unavailable);
}

// Five real owner threads and twenty peer workers, with real protobuf bytes on
// every injected link. Memory-backed persistence; this is not a socket/disk test.
class ThreadedCluster {
 public:
  struct Replica {
    explicit Replica(NodeId id) : wire(codec(id)), ledger{0,ledger_genesis().accounts} {}
    WireCodec wire;
    std::mutex mutex;
    State disk;
    LedgerState ledger;
    std::map<Hash,LedgerReceipt> receipts;
    std::unique_ptr<PeerDispatcher> transport;
    std::unique_ptr<Runtime> runtime;
    std::unique_ptr<RaftIngress> ingress;
    std::unique_ptr<LedgerSubmission> submission;
  };
  explicit ThreadedCluster(bool guarded = false) {
    for (NodeId id = 1; id <= 5; ++id) nodes.emplace(id,std::make_unique<Replica>(id));
    for (auto& [id,node] : nodes) {
      node->transport = std::make_unique<PeerDispatcher>(node->wire,
        [&,from=id](NodeId peer, const WireRequest& wire, RpcDeadline deadline, std::stop_token token) -> RpcResult {
          const auto separated = [&] { const auto mask = minority.load(); return ((mask >> from)&1U) != ((mask >> peer)&1U); };
          if (separated()) return {RpcCode::unavailable};
          // Serialize and parse actual generated messages, not typed Core envelopes.
          const auto bytes = serialize_raft_wire(wire);
          const WireRequest request = std::visit([&](const auto& value) -> WireRequest {
            std::decay_t<decltype(value)> parsed;
            if (!parsed.ParseFromString(bytes)) throw std::runtime_error("request parse failed");
            return parsed;
          },wire);
          auto result = nodes.at(peer)->ingress->respond(request,deadline,[&] { return token.stop_requested() || separated(); });
          if (result.response) {
            const auto response_bytes = serialize_raft_wire(*result.response);
            result.response = std::visit([&](const auto& value) -> WireResponse {
              std::decay_t<decltype(value)> parsed;
              if (!parsed.ParseFromString(response_bytes)) throw std::runtime_error("response parse failed");
              return parsed;
            },*result.response);
          }
          return result;
        },2s);
      auto config = raft_config(); config.id = id; config.seed = id*991;
      auto* replica = node.get();
      const ReceiptLookup read = [replica](const Hash& id) -> std::optional<LedgerReceipt> {
        std::lock_guard lock(replica->mutex); const auto found = replica->receipts.find(id);
        return found == replica->receipts.end() ? std::nullopt : std::optional{found->second};
      };
      node->runtime = std::make_unique<Runtime>(config,node->disk,[replica](const State& state) {
        std::lock_guard lock(replica->mutex);
        validate_persisted_transition(replica->wire.manifest(),replica->disk,state);
        replica->disk = decode_state_record(replica->wire.manifest(),encode_state_record(replica->wire.manifest(),state));
      },[replica](Index commit) {
        std::lock_guard lock(replica->mutex);
        const ReceiptLookup lookup = [replica](const Hash& id) -> std::optional<LedgerReceipt> {
          const auto found = replica->receipts.find(id);
          return found == replica->receipts.end() ? std::nullopt : std::optional{found->second};
        };
        while (replica->ledger.applied_index < commit) {
          const auto plan = plan_committed_entry(ledger_genesis(),replica->ledger,replica->disk,replica->ledger.applied_index+1,lookup);
          for (const auto& receipt : plan.new_receipts) replica->receipts.emplace(receipt.transaction_id,receipt);
          replica->ledger = plan.state;
        }
        return replica->ledger.applied_index;
      },[replica](const Envelope& message) { return replica->transport->try_enqueue(message); },manual(),[&] { return now.load(); },
        guarded ? ledger_proposal_policy(ledger_genesis(),[replica] {
          std::lock_guard lock(replica->mutex); return replica->ledger;
        },read) : ProposalCheck{});
      node->transport->bind(node->runtime->handle());
      node->ingress = std::make_unique<RaftIngress>(node->wire,node->runtime->handle(),IngressOptions{16,2s,1ms});
      if (guarded) node->submission = std::make_unique<LedgerSubmission>(node->runtime->handle(),read);
    }
  }
  ~ThreadedCluster() {
    for (auto& [id,node] : nodes) { (void)id; node->ingress->close(); node->transport->request_stop(); }
    for (auto& [id,node] : nodes) { (void)id; node->transport->stop(); }
    for (auto& [id,node] : nodes) { (void)id; node->runtime->stop(); }
  }
  void step() {
    now += 10;
    for (auto& [id,node] : nodes) { (void)id; (void)await(node->runtime->handle().tick()); }
    const auto deadline = std::chrono::steady_clock::now()+3s;
    for (auto& [id,node] : nodes) { (void)id;
      if (!node->transport->wait_idle(deadline)) throw std::runtime_error("transport did not drain");
      if (node->runtime->failure()) std::rethrow_exception(node->runtime->failure());
      if (node->transport->failure()) std::rethrow_exception(node->transport->failure());
    }
  }
  void until(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now()+10s;
    for (unsigned i = 0; i < 500 && std::chrono::steady_clock::now() < deadline; ++i) { step(); if (predicate()) return; }
    throw std::runtime_error("cluster convergence deadline missed");
  }
  NodeId leader() const {
    for (const auto& [id,node] : nodes) {
      const auto status = node->runtime->status();
      if (!(minority.load() & (1U<<id)) && status.role == Role::leader && status.commit > 0 && status.applied == status.commit) {
        std::lock_guard lock(node->mutex);
        if (node->disk.log.at(status.commit-1).term == status.term) return id;
      }
    }
    return 0;
  }
  bool applied(Index index, bool majority_only = false) const {
    for (const auto& [id,node] : nodes)
      if ((!majority_only || !(minority.load() & (1U<<id))) && node->runtime->status().applied < index) return false;
    return true;
  }
  std::map<NodeId,std::unique_ptr<Replica>> nodes;
  std::atomic<unsigned> minority{0}; std::atomic<Millis> now{0};
};
TEST(RaftThreadedCluster, PartitionedLeaderCannotCommitAndFiveReplicasConvergeWithIdempotentTransfers) {
  ThreadedCluster cluster; cluster.until([&] { return cluster.leader() != 0; });
  const auto old = cluster.leader();
  auto proposal = await(cluster.nodes.at(old)->runtime->handle().propose(encode_transfer_command({ledger_transaction()})));
  ASSERT_EQ(proposal.completion,Completion::processed); ASSERT_TRUE(proposal.proposed_index);
  cluster.until([&] { return cluster.applied(*proposal.proposed_index); });
  const auto committed = cluster.nodes.at(old)->runtime->status().commit;
  cluster.minority = (1U<<old) | (1U<<(old == 1 ? 2 : 1));
  (void)await(cluster.nodes.at(old)->runtime->handle().propose(encode_transfer_command({ledger_transaction(1,30)})));
  cluster.until([&] { return cluster.leader() != 0 && cluster.leader() != old; });
  EXPECT_EQ(cluster.nodes.at(old)->runtime->status().commit,committed);
  proposal = await(cluster.nodes.at(cluster.leader())->runtime->handle().propose(
    encode_transfer_command({ledger_transaction(),ledger_transaction(1,20)})));
  ASSERT_EQ(proposal.completion,Completion::processed); ASSERT_TRUE(proposal.proposed_index);
  cluster.until([&] { return cluster.applied(*proposal.proposed_index,true); });
  EXPECT_EQ(cluster.nodes.at(old)->runtime->status().commit,committed);
  cluster.minority = 0; cluster.until([&] { return cluster.applied(*proposal.proposed_index); });
  // Stop all owners before inspecting final state and committed-prefix agreement.
  for (auto& [id,node] : cluster.nodes) { (void)id; node->runtime->stop(); }
  for (const auto& [id,node] : cluster.nodes) {
    SCOPED_TRACE(id); EXPECT_EQ(node->ledger.accounts[0].balance,70U);
    EXPECT_EQ(node->ledger.accounts[0].nonce,2U); EXPECT_EQ(node->receipts.size(),2U);
    EXPECT_FALSE(node->receipts.contains(ledger_transaction(1,30).id));
    for (const auto& [other_id,other] : cluster.nodes) {
      (void)other_id;
      for (Index i = 0; i < std::min(node->disk.commit,other->disk.commit); ++i) EXPECT_EQ(node->disk.log[i],other->disk.log[i]);
    }
  }
}
TEST(RaftThreadedCluster, GuardedSubmissionConfirmsAppliedTransfersAndConcurrentRetriesAcrossFiveReplicas) {
  ThreadedCluster cluster(true); cluster.until([&] { return cluster.leader() != 0; });
  auto& leader = *cluster.nodes.at(cluster.leader());
  const auto transaction = ledger_transaction();
  std::vector<std::future<SubmissionResult>> callers;
  for (unsigned i = 0; i < 5; ++i) callers.push_back(std::async(std::launch::async,[&] {
    return leader.submission->submit(transaction,std::chrono::steady_clock::now()+2s);
  }));
  cluster.until([&] {
    return std::all_of(callers.begin(),callers.end(),[](auto& call) { return call.wait_for(0ms) == std::future_status::ready; });
  });
  unsigned confirmed = 0;
  for (auto& call : callers) {
    const auto result = call.get(); EXPECT_TRUE(result.code == SubmissionCode::confirmed || result.code == SubmissionCode::busy);
    if (result.code == SubmissionCode::confirmed) { ++confirmed; ASSERT_TRUE(result.receipt); EXPECT_EQ(result.receipt->log_index,2U); }
  }
  EXPECT_GT(confirmed,0U); cluster.until([&] { return cluster.applied(2); });
  for (const auto& [id,node] : cluster.nodes) {
    SCOPED_TRACE(id);
    const auto retry = node->submission->submit(transaction,std::chrono::steady_clock::now()+1s);
    EXPECT_EQ(retry.code,SubmissionCode::confirmed); ASSERT_TRUE(retry.receipt); EXPECT_EQ(retry.receipt->log_index,2U);
    std::lock_guard lock(node->mutex);
    EXPECT_EQ(node->ledger.accounts[0].balance,90U); EXPECT_EQ(node->ledger.accounts[0].nonce,1U);
    EXPECT_EQ(node->receipts.size(),1U); EXPECT_EQ(node->disk.log.size(),2U);
  }
}
} // namespace
