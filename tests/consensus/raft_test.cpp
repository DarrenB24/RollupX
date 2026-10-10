#include "rollupx/raft.hpp"
#include "../ledger_fixture.hpp"
#include "../raft_store_fixture.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <deque>
#include <memory>

namespace {
using namespace rollupx::raft;
Config config(NodeId id = 1) { return {id, {1,2,3,4,5}, 50, 300, 600, id*991}; }
class Raft : public ::testing::Test {
 protected:
  State disk;
  unsigned writes = 0, fail_write = 0;
  bool fail_after_write = false;
  std::unique_ptr<Core> core;
  void open(State state = {}) {
    disk = std::move(state);
    core = std::make_unique<Core>(config(), disk, [this](const State& next) {
      ++writes;
      if (writes == fail_write && !fail_after_write) throw std::runtime_error("write failed before effect");
      disk = next;
      if (writes == fail_write) throw std::runtime_error("ambiguous write failure");
    });
  }
  std::vector<Envelope> elect() {
    (void)core->tick(600); const auto term = core->state().term;
    (void)core->receive({2,1,VoteResponse{term,true}}, 600);
    return core->receive({3,1,VoteResponse{term,true}}, 600);
  }
  std::vector<Envelope> acknowledge(const Envelope& envelope, Millis now = 600) {
    const auto& request = std::get<AppendRequest>(envelope.message);
    return core->receive({envelope.to,1,AppendResponse{request.term,request.rpc,true,
      request.previous_index+request.entries.size(),0}}, now);
  }
};
TEST_F(Raft, SelfVoteAndLeaderBarrierPersistBeforeMessages) {
  open(); const auto requests = core->tick(600);
  ASSERT_EQ(requests.size(),4U); EXPECT_EQ(disk.term,1U); EXPECT_EQ(disk.voted_for,1U);
  EXPECT_EQ(core->role(),Role::candidate); EXPECT_EQ(core->elections(),1U);
  EXPECT_TRUE(core->receive({2,1,VoteResponse{1,true}},600).empty());
  EXPECT_TRUE(core->receive({2,1,VoteResponse{1,true}},600).empty());
  EXPECT_EQ(core->role(),Role::candidate); // Duplicate votes never form quorum.
  const auto replication = core->receive({3,1,VoteResponse{1,true}},600);
  EXPECT_EQ(core->role(),Role::leader); ASSERT_EQ(disk.log.size(),1U); EXPECT_TRUE(disk.log[0].command.empty());
  EXPECT_THROW(core->propose("command",600),std::runtime_error);
  (void)acknowledge(replication[0]); EXPECT_EQ(disk.commit,0U);
  (void)acknowledge(replication[1]); EXPECT_EQ(disk.commit,1U);
  EXPECT_NO_THROW(core->propose("command",600)); EXPECT_EQ(disk.log.back().command,"command");
}
TEST_F(Raft, OneVotePerTermSurvivesRestartAndLogFreshnessUsesTermFirst) {
  open({3,{},{{1,"old"},{3,"new"}},0});
  auto response = core->receive({2,1,VoteRequest{4,3,2}},0);
  EXPECT_FALSE(std::get<VoteResponse>(response[0].message).granted); EXPECT_EQ(disk.term,4U);
  response = core->receive({2,1,VoteRequest{4,2,3}},1);
  EXPECT_TRUE(std::get<VoteResponse>(response[0].message).granted); EXPECT_EQ(disk.voted_for,2U);
  const auto saved = disk; open(saved);
  response = core->receive({3,1,VoteRequest{4,2,3}},2);
  EXPECT_FALSE(std::get<VoteResponse>(response[0].message).granted);
  response = core->receive({2,1,VoteRequest{4,2,3}},3);
  EXPECT_TRUE(std::get<VoteResponse>(response[0].message).granted);
}
TEST_F(Raft, HigherTermsStepDownAndStaleResponsesCannotElect) {
  open(); (void)elect(); EXPECT_EQ(core->role(),Role::leader);
  EXPECT_TRUE(core->receive({4,1,VoteResponse{2,false}},601).empty());
  EXPECT_EQ(core->role(),Role::follower); EXPECT_EQ(disk.term,2U); EXPECT_FALSE(disk.voted_for);
  (void)core->receive({2,1,VoteResponse{1,true}},602);
  (void)core->receive({3,1,VoteResponse{1,true}},602);
  EXPECT_EQ(core->role(),Role::follower);
}
TEST_F(Raft, DeniedVotesAndStaleHeartbeatsDoNotPostponeElection) {
  open({3,{},{{3,"current"}},0});
  for (Millis time = 50; time <= 550; time += 50) {
    (void)core->receive({2,1,VoteRequest{3,0,0}},time);
    (void)core->receive({2,1,AppendRequest{2,1,0,0,{},0}},time);
  }
  EXPECT_FALSE(core->tick(600).empty()); EXPECT_EQ(core->role(),Role::candidate);
}
TEST_F(Raft, FollowerRepairsOnlyConflictingSuffixAndCapsHeartbeatCommit) {
  open({3,{},{{1,"kept"},{2,"divergent"},{2,"tail"}},1});
  auto replies = core->receive({2,1,AppendRequest{3,1,1,1,{},3}},0);
  EXPECT_TRUE(std::get<AppendResponse>(replies[0].message).success);
  EXPECT_EQ(disk.commit,1U); EXPECT_EQ(disk.log.size(),3U); // Unmatched suffix is not committed or truncated.
  replies = core->receive({2,1,AppendRequest{3,2,1,1,{{3,"replacement"}},2}},1);
  EXPECT_TRUE(std::get<AppendResponse>(replies[0].message).success);
  ASSERT_EQ(disk.log.size(),2U); EXPECT_EQ(disk.log[1].command,"replacement"); EXPECT_EQ(disk.commit,2U);
  const auto count = writes;
  (void)core->receive({2,1,AppendRequest{3,2,1,1,{{3,"replacement"}},2}},2);
  EXPECT_EQ(writes,count); // Exact duplicate needs no new persistent mutation.
}
TEST_F(Raft, RejectsMissingPreviousEntryAndReturnsConflictStart) {
  open({3,{},{{1,"a"},{2,"b"},{2,"c"}},0});
  auto responses = core->receive({2,1,AppendRequest{3,1,5,2,{},0}},0);
  auto reply = std::get<AppendResponse>(responses[0].message);
  EXPECT_FALSE(reply.success); EXPECT_EQ(reply.conflict_index,4U);
  responses = core->receive({2,1,AppendRequest{3,2,3,3,{},0}},1);
  reply = std::get<AppendResponse>(responses[0].message);
  EXPECT_FALSE(reply.success); EXPECT_EQ(reply.conflict_index,2U); EXPECT_EQ(writes,0U);
}
TEST_F(Raft, ConflictingCommittedEntryOrSameTermPayloadFailsClosed) {
  open({2,{},{{1,"committed"}},1});
  EXPECT_THROW(core->receive({2,1,AppendRequest{2,1,0,0,{{2,"bad"}},1}},0),std::runtime_error);
  EXPECT_TRUE(core->failed()); EXPECT_EQ(disk.log[0].command,"committed");
  EXPECT_THROW(core->tick(1),std::runtime_error);
  open({2,{},{{2,"same-term"}},0});
  EXPECT_THROW(core->receive({2,1,AppendRequest{2,1,0,0,{{2,"different"}},0}},0),std::runtime_error);
  EXPECT_TRUE(core->failed());
}
TEST_F(Raft, OldTermMajorityNeedsCurrentTermBarrier) {
  State initial{1,{},std::vector<Entry>(20,{1,"old"}),0}; open(initial);
  const auto initial_requests = elect(); ASSERT_EQ(disk.term,2U);
  std::vector<Envelope> tails;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& request = std::get<AppendRequest>(initial_requests[i].message);
    const auto retry = core->receive({initial_requests[i].to,1,AppendResponse{2,request.rpc,false,0,1}},600);
    ASSERT_EQ(retry.size(),1U); EXPECT_EQ(std::get<AppendRequest>(retry[0].message).entries.size(),16U);
    auto tail = acknowledge(retry[0]); ASSERT_EQ(tail.size(),1U); tails.push_back(tail[0]);
  }
  EXPECT_EQ(disk.commit,0U); // Two followers have 16 entries, all from the old term.
  (void)acknowledge(tails[0]); EXPECT_EQ(disk.commit,0U);
  (void)acknowledge(tails[1]); EXPECT_EQ(disk.commit,21U);
}
TEST_F(Raft, DelayedAndDuplicateRepliesCannotAdvanceProgressOrRefreshQuorum) {
  open(); const auto requests = elect();
  auto response = std::get<AppendRequest>(requests[0].message);
  EXPECT_TRUE(core->receive({2,1,AppendResponse{1,response.rpc+100,true,1,0}},600).empty());
  (void)acknowledge(requests[0]); (void)acknowledge(requests[1]);
  EXPECT_EQ(disk.commit,1U);
  (void)acknowledge(requests[0],1199); // Old rpc is no longer in flight.
  (void)core->tick(1200); EXPECT_EQ(core->role(),Role::follower);
  EXPECT_THROW(core->propose("minority",1200),std::runtime_error);
}
TEST_F(Raft, StorageFailuresEmitNoAcknowledgmentAndRequireRecovery) {
  for (const bool ambiguous : {false,true}) {
    writes = 0; fail_write = 2; fail_after_write = ambiguous; open();
    // First persist advances term, second persists the granted vote.
    EXPECT_THROW(core->receive({2,1,VoteRequest{1,0,0}},0),std::runtime_error);
    EXPECT_TRUE(core->failed()); EXPECT_THROW(core->tick(600),std::runtime_error);
    EXPECT_EQ(disk.voted_for.has_value(),ambiguous);
    const auto recovered = disk; fail_write = 0; open(recovered);
    const auto reply = core->receive({3,1,VoteRequest{1,0,0}},1);
    EXPECT_EQ(std::get<VoteResponse>(reply[0].message).granted,!ambiguous);
  }
}
TEST_F(Raft, AppendAndCommitArePersistedTogetherBeforeSuccess) {
  open(); fail_write = 2; fail_after_write = true;
  EXPECT_THROW(core->receive({2,1,AppendRequest{1,1,0,0,{{1,"command"}},1}},0),std::runtime_error);
  EXPECT_TRUE(core->failed()); EXPECT_EQ(disk.commit,1U); ASSERT_EQ(disk.log.size(),1U);
  const auto recovered = disk; fail_write = 0; open(recovered);
  EXPECT_EQ(core->state().commit,1U); EXPECT_EQ(core->state().log[0].command,"command");
}
TEST_F(Raft, LeaderCommitMarkerFailureCannotAnnounceCommitment) {
  open(); const auto requests = elect(); (void)acknowledge(requests[0]);
  fail_write = writes+1; fail_after_write = true;
  EXPECT_THROW(acknowledge(requests[1]),std::runtime_error);
  EXPECT_TRUE(core->failed()); EXPECT_EQ(disk.commit,1U);
  EXPECT_THROW(core->propose("after-storage-failure",601),std::runtime_error);
  const auto recovered = disk; fail_write = 0; open(recovered);
  EXPECT_EQ(core->state().commit,1U); EXPECT_EQ(core->role(),Role::follower);
}
TEST_F(Raft, RejectsMalformedTrafficBeforePersistingHigherTerm) {
  open();
  EXPECT_THROW(core->receive({99,1,VoteRequest{9,0,0}},0),std::invalid_argument);
  EXPECT_THROW(core->receive({2,1,VoteRequest{9,0,1}},0),std::invalid_argument);
  EXPECT_THROW(core->receive({2,1,AppendRequest{9,1,0,0,{{10,"future"}},0}},0),std::invalid_argument);
  EXPECT_THROW(core->receive({2,1,AppendRequest{9,1,0,0,{{9,std::string(kMaxCommandBytes+1,'x')}},0}},0),std::invalid_argument);
  EXPECT_EQ(writes,0U); EXPECT_EQ(disk.term,0U);
  (void)core->tick(1); EXPECT_THROW(core->tick(0),std::invalid_argument);
}
TEST_F(Raft, RejectsInvalidRecoveredStateAndLogCapacity) {
  EXPECT_THROW(open({1,99,{},0}),std::invalid_argument);
  EXPECT_THROW(open({1,{},{{2,"future"}},0}),std::invalid_argument);
  EXPECT_THROW(open({1,{}, {},1}),std::invalid_argument);
  EXPECT_THROW(open({2,{},{{2,"new"},{1,"old"}},0}),std::invalid_argument);
  EXPECT_THROW(open({1,{},std::vector<Entry>(kMaxLogEntries+1,{1,{}}),0}),std::invalid_argument);
  EXPECT_THROW(open({1,{},std::vector<Entry>(50,{1,std::string(kMaxCommandBytes,'x')}),0}),std::length_error);
}

// A deterministic transport and atomic memory recorder verify protocol decisions.
// They do not stand in for real gRPC, RocksDB WAL, fsync, or process-crash tests.
class Cluster {
 public:
  explicit Cluster(std::uint64_t fault_seed = 0) : fault_seed(fault_seed), random(fault_seed) {
    for (NodeId id = 1; id <= 5; ++id) restart(id);
  }
  void restart(NodeId id) {
    auto options = config(id); options.seed += fault_seed;
    nodes[id] = std::make_unique<Core>(options, disks[id], [this,id](const State& next) {
      auto identity = rollupx::test::raft_manifest(); identity.node_id = id;
      validate_persisted_transition(identity,disks[id],next);
      // Exercise the actual record boundary on every simulated persistent effect.
      disks[id] = decode_state_record(identity,encode_state_record(identity,next));
    }, clock(id));
  }
  Millis clock(NodeId id) const { return now + (fault_seed == 0 ? 0 : id*7); }
  void enqueue(std::vector<Envelope> messages) {
    for (auto& message : messages) {
      if (fault_seed != 0 && random()%10 == 0) queue.push_back({now+random()%150,message});
      queue.push_back({now+(fault_seed == 0 ? 0 : random()%90),std::move(message)});
    }
  }
  void drain() {
    std::size_t steps = 0;
    while (true) {
      const auto ready = std::find_if(queue.begin(),queue.end(),[&](const auto& item) { return item.due <= now; });
      if (ready == queue.end()) break;
      if (++steps > 10000) throw std::runtime_error("message loop exceeded deterministic budget");
      auto message = std::move(ready->message); queue.erase(ready);
      if (fault_seed != 0 && random()%20 == 0) continue;
      if (!nodes[message.to] || blocked.contains({message.from,message.to})) continue;
      enqueue(nodes[message.to]->receive(message,clock(message.to))); check_safety();
    }
  }
  void advance(Millis duration) {
    for (Millis elapsed = 0; elapsed < duration; elapsed += 10) {
      now += 10;
      for (auto& [id,node] : nodes) if (node) enqueue(node->tick(clock(id)));
      drain(); check_safety();
    }
  }
  NodeId leader(const std::set<NodeId>& excluded = {}) const {
    NodeId result = 0;
    for (const auto& [id,node] : nodes)
      if (node && !excluded.contains(id) && node->role() == Role::leader) {
        if (result != 0) throw std::runtime_error("more than one active leader in selected partition");
        result = id;
      }
    return result;
  }
  void isolate(NodeId id) {
    for (NodeId other = 1; other <= 5; ++other) if (id != other) { blocked.emplace(id,other); blocked.emplace(other,id); }
  }
  void submit(NodeId id, std::string value) { enqueue(nodes.at(id)->propose(std::move(value),clock(id))); drain(); }
  void check_safety() {
    std::map<Term,NodeId> leaders;
    for (const auto& [id,node] : nodes) if (node && node->role() == Role::leader) {
      const auto [it,inserted] = leaders.emplace(node->state().term,id);
      if (!inserted && it->second != id) throw std::runtime_error("two leaders in one term");
    }
    for (const auto& [id,state] : disks) {
      (void)id;
      for (Index index = 1; index <= state.commit; ++index) {
        const auto [it,inserted] = committed.emplace(index,state.log.at(index-1));
        if (!inserted && it->second != state.log.at(index-1)) throw std::runtime_error("conflicting committed history");
      }
    }
  }
  Millis now = 0;
  std::map<NodeId,State> disks;
  std::map<NodeId,std::unique_ptr<Core>> nodes;
  struct Delivery { Millis due; Envelope message; };
  std::deque<Delivery> queue;
  std::set<std::pair<NodeId,NodeId>> blocked;
  std::map<Index,Entry> committed;
  std::uint64_t fault_seed;
  std::mt19937_64 random;
};
TEST(RaftCluster, FiveNodesCommitAndTwoCrashesRetainQuorum) {
  Cluster cluster; cluster.advance(1000); const auto leader = cluster.leader(); ASSERT_NE(leader,0U);
  cluster.submit(leader,"first"); cluster.advance(100);
  unsigned crashed = 0;
  for (auto& [id,node] : cluster.nodes) if (id != leader && crashed < 2) { node.reset(); ++crashed; }
  cluster.submit(leader,"after-two-crashes"); cluster.advance(100);
  for (const auto& [id,node] : cluster.nodes) if (node) {
    EXPECT_EQ(cluster.disks[id].commit,3U); EXPECT_EQ(cluster.disks[id].log[2].command,"after-two-crashes");
  }
}
TEST(RaftCluster, LeaderCrashElectionAndRestartPreserveCommittedHistory) {
  Cluster cluster; cluster.advance(1000); const auto original = cluster.leader(); ASSERT_NE(original,0U);
  cluster.submit(original,"survives"); cluster.advance(100); cluster.nodes[original].reset();
  cluster.advance(1000); const auto next = cluster.leader(); ASSERT_NE(next,0U); EXPECT_NE(next,original);
  cluster.submit(next,"new-leader"); cluster.restart(original); cluster.advance(1000);
  for (const auto& [id,state] : cluster.disks) { (void)id; EXPECT_EQ(state.log,cluster.disks[next].log); EXPECT_EQ(state.commit,4U); }
}
TEST(RaftCluster, MinorityCannotCommitAndDivergentSuffixRepairsAfterHealing) {
  Cluster cluster; cluster.advance(1000); const auto original = cluster.leader(); ASSERT_NE(original,0U);
  const auto before = cluster.disks[original].commit;
  cluster.isolate(original); cluster.submit(original,"uncommitted-minority");
  EXPECT_EQ(cluster.disks[original].commit,before);
  cluster.advance(1000); EXPECT_NE(cluster.nodes[original]->role(),Role::leader);
  const auto majority = cluster.leader({original}); ASSERT_NE(majority,0U);
  EXPECT_THROW(cluster.submit(original,"stale-leader"),std::runtime_error);
  cluster.submit(majority,"majority-command"); cluster.blocked.clear(); cluster.advance(1000);
  for (const auto& [id,state] : cluster.disks) {
    (void)id; EXPECT_EQ(state.log,cluster.disks[majority].log);
    for (const auto& entry : state.log) EXPECT_NE(entry.command,"uncommitted-minority");
  }
}
TEST(RaftCluster, SplitVoteRecoversWithSeededElectionTimeouts) {
  Cluster cluster;
  for (NodeId id = 1; id <= 5; ++id) cluster.isolate(id);
  cluster.advance(1500); EXPECT_EQ(cluster.leader(),0U);
  for (const auto& [id,state] : cluster.disks) { (void)id; EXPECT_EQ(state.commit,0U); }
  cluster.blocked.clear(); cluster.advance(1500); ASSERT_NE(cluster.leader(),0U);
  cluster.submit(cluster.leader(),"after-split-vote"); cluster.advance(100);
  for (const auto& [id,state] : cluster.disks) { (void)id; EXPECT_EQ(state.commit,2U); }
}
TEST(RaftCluster, TwoVersusThreePartitionCannotCommitInMinority) {
  Cluster cluster; cluster.advance(1000); const auto original = cluster.leader(); ASSERT_NE(original,0U);
  const NodeId companion = original == 1 ? 2 : 1;
  const std::set<NodeId> minority{original,companion};
  for (NodeId a = 1; a <= 5; ++a) for (NodeId b = 1; b <= 5; ++b)
    if (minority.contains(a) != minority.contains(b)) cluster.blocked.emplace(a,b);
  cluster.submit(original,"minority-only");
  EXPECT_EQ(cluster.disks[original].commit,1U); EXPECT_EQ(cluster.disks[companion].commit,1U);
  cluster.advance(1200); const auto majority = cluster.leader(minority); ASSERT_NE(majority,0U);
  cluster.submit(majority,"three-node-quorum"); cluster.advance(100);
  for (NodeId id = 1; id <= 5; ++id) EXPECT_EQ(cluster.disks[id].commit,minority.contains(id) ? 1U : 3U);
  cluster.blocked.clear(); cluster.advance(1500);
  for (const auto& [id,state] : cluster.disks) { (void)id; EXPECT_EQ(state.log,cluster.disks[majority].log); }
}
TEST(RaftCluster, ReplicasExecuteOnlyCommittedSignedTransfersAndDeduplicateRetries) {
  Cluster cluster; cluster.advance(1000); const auto first_leader = cluster.leader(); ASSERT_NE(first_leader,0U);
  const auto command = [](const rollupx::SignedTransaction& transaction) {
    return rollupx::encode_transfer_command({transaction});
  };
  const auto first = rollupx::test::ledger_transaction();
  cluster.submit(first_leader,command(first)); cluster.advance(100);
  cluster.nodes[first_leader].reset(); cluster.advance(1000);
  const auto second_leader = cluster.leader(); ASSERT_NE(second_leader,0U);
  cluster.submit(second_leader,command(first)); // Retry after losing leader/client connection.
  cluster.submit(second_leader,command(rollupx::test::ledger_transaction(1,20)));
  cluster.restart(first_leader); cluster.advance(1000);
  const auto genesis = rollupx::test::ledger_genesis();
  for (const auto& [id,state] : cluster.disks) {
    SCOPED_TRACE(id); rollupx::LedgerState applied{0,genesis.accounts};
    std::map<rollupx::Hash,rollupx::LedgerReceipt> receipts;
    const rollupx::ReceiptLookup lookup = [&](const rollupx::Hash& key) -> std::optional<rollupx::LedgerReceipt> {
      const auto found = receipts.find(key); return found == receipts.end() ? std::nullopt : std::optional{found->second};
    };
    for (Index index = 1; index <= state.commit; ++index) {
      const auto plan = rollupx::plan_committed_entry(genesis,applied,state,index,lookup);
      for (const auto& receipt : plan.new_receipts) receipts.emplace(receipt.transaction_id,receipt);
      applied = plan.state;
      EXPECT_NO_THROW(rollupx::audit_raft_application(genesis,applied,state,lookup));
    }
    EXPECT_EQ(applied.accounts[0].balance,70U); EXPECT_EQ(applied.accounts[0].nonce,2U);
    EXPECT_EQ(applied.accounts[1].balance,30U); EXPECT_EQ(receipts.size(),2U);
    EXPECT_EQ(applied.applied_index,state.commit);
  }
}
TEST(RaftCluster, SeededDelayLossDuplicationRestartAndPartitionsPreserveSafety) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SCOPED_TRACE(seed); Cluster cluster(seed); cluster.advance(2500);
    const auto first = cluster.leader(); ASSERT_NE(first,0U);
    cluster.submit(first,"before-fault"); cluster.advance(1000);
    cluster.nodes[first].reset(); cluster.advance(2500);
    const auto second = cluster.leader(); ASSERT_NE(second,0U);
    cluster.submit(second,"after-crash"); cluster.restart(first); cluster.advance(1500);
    cluster.isolate(second); cluster.advance(2500);
    const auto third = cluster.leader({second}); ASSERT_NE(third,0U);
    cluster.submit(third,"after-partition"); cluster.advance(1000);
    cluster.blocked.clear(); cluster.advance(3000);
    const auto stable = cluster.leader(); ASSERT_NE(stable,0U);
    cluster.submit(stable,"final-barrier"); cluster.advance(1500);
    for (const auto& [id,state] : cluster.disks) {
      SCOPED_TRACE(id);
      EXPECT_EQ(state.log,cluster.disks[stable].log); EXPECT_EQ(state.commit,state.log.size());
      for (const auto* value : {"before-fault","after-crash","after-partition","final-barrier"})
        EXPECT_TRUE(std::any_of(state.log.begin(),state.log.end(),[&](const Entry& entry) { return entry.command == value; }));
    }
  }
}
} // namespace
