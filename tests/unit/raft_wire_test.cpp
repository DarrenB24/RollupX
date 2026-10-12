#include "rollupx/raft_wire.hpp"
#include "../raft_store_fixture.hpp"
#include <algorithm>
#include <deque>
#include <gtest/gtest.h>
#include <google/protobuf/unknown_field_set.h>
#include <memory>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
namespace pb = rollupx::internal::v1;
const std::vector<std::string> members{"n1","n2","n3","n4","n5"};
Hash genesis() { return raft_ledger_manifest().genesis_digest; }
WireCodec codec(NodeId id) { return {"test",members,genesis(),id}; }
Envelope vote() { return {1,2,VoteRequest{3,2,2}}; }
Envelope append() { return {1,2,AppendRequest{3,42,1,1,{{2,""},{3,encode_transfer_command({ledger_transaction()})}},3}}; }
void unknown(google::protobuf::Message& message) {
  message.GetReflection()->MutableUnknownFields(&message)->AddVarint(100,1);
}
TEST(RaftWire, ConfigurationBindsNamesClusterAndGenesisButNotLocalNode) {
  EXPECT_EQ(codec(1).configuration_id(),codec(2).configuration_id());
  auto different = genesis(); different[0] ^= 1;
  EXPECT_NE(codec(1).configuration_id(),WireCodec("test",members,different,1).configuration_id());
  EXPECT_NE(codec(1).configuration_id(),WireCodec("other",members,genesis(),1).configuration_id());
  auto names = members; names.back() = "n6";
  EXPECT_NE(codec(1).configuration_id(),WireCodec("test",names,genesis(),1).configuration_id());
  std::swap(names[0],names[1]); EXPECT_THROW(WireCodec("test",names,genesis(),1),std::invalid_argument);
  EXPECT_THROW(WireCodec("test",members,Hash{},1),std::invalid_argument);
  EXPECT_THROW(WireCodec("test",members,genesis(),0),std::invalid_argument);
  names = members; names[1] = names[0]; EXPECT_THROW(WireCodec("test",names,genesis(),1),std::invalid_argument);
  names = members; names.back() = std::string(129,'z'); EXPECT_THROW(WireCodec("test",names,genesis(),1),std::invalid_argument);
}
TEST(RaftWire, RequestsAndRepliesRoundTripThroughRealProtobufBytes) {
  for (const auto& request : {vote(),append()}) {
    const auto a = codec(1), b = codec(2);
    const auto wire = a.encode_request(request);
    const auto received = b.parse_request(std::holds_alternative<VoteRequest>(request.message) ? WireMethod::vote : WireMethod::append,
      serialize_raft_wire(wire));
    EXPECT_EQ(received.from,1U); EXPECT_EQ(received.to,2U);
    EXPECT_EQ(serialize_raft_wire(a.encode_request(received)),serialize_raft_wire(wire));
    Envelope response{2,1,VoteResponse{3,true}};
    if (std::holds_alternative<AppendRequest>(request.message)) response.message = AppendResponse{3,42,true,3,0};
    const auto reply = b.encode_response(response,received);
    const auto decoded = a.parse_response(serialize_raft_wire(reply),request);
    EXPECT_EQ(serialize_raft_wire(b.encode_response(decoded,received)),serialize_raft_wire(reply));
  }
}
TEST(RaftWire, RecoveredNamedManifestIsTheSameIdentityForStorageAndTransport) {
  const auto original = codec(1);
  const auto recovered = decode_manifest(encode_manifest(original.manifest()));
  WireCodec reopened(recovered);
  EXPECT_EQ(reopened.configuration_id(),original.configuration_id());
  EXPECT_EQ(serialize_raft_wire(reopened.encode_request(vote())),serialize_raft_wire(original.encode_request(vote())));
  EXPECT_EQ(reopened.manifest().members,(std::vector<NodeId>{1,2,3,4,5}));
  EXPECT_EQ(reopened.manifest().member_names,members);
  EXPECT_THROW(WireCodec{raft_manifest()},std::invalid_argument);
  auto altered = recovered; altered.member_names.back() = "n6";
  EXPECT_THROW(codec(2).decode_request(WireCodec(altered).encode_request(vote())),std::invalid_argument);
  EXPECT_THROW(decode_state_record(altered,encode_state_record(recovered,{3,2,{},0})),std::invalid_argument);
}
TEST(RaftWire, RejectsMissingForeignOrAlteredRequestContext) {
  const auto original = std::get<pb::RequestVoteRequest>(codec(1).encode_request(vote()));
  for (int change = 0; change < 10; ++change) {
    SCOPED_TRACE(change); auto message = original;
    switch (change) {
      case 0: message.clear_context(); break;
      case 1: message.set_cluster_id("foreign"); break;
      case 2: message.set_candidate_id("n3"); break;
      case 3: message.mutable_context()->set_sender_id("unknown"); break;
      case 4: message.mutable_context()->set_recipient_id("n3"); break;
      case 5: message.mutable_context()->set_configuration_id(std::string(32,'x')); break;
      case 6: message.mutable_context()->set_protocol_version(2); break;
      case 7: message.mutable_context()->set_request_term(4); break;
      case 8: message.mutable_context()->set_rpc_id(1); break;
      case 9: message.mutable_context()->set_sender_id("n2"); break;
    }
    EXPECT_THROW(codec(2).decode_request(message),std::invalid_argument);
  }
}
TEST(RaftWire, RejectsUnknownFieldsAtEveryMessageNestingLevel) {
  for (int level = 0; level < 3; ++level) {
    auto message = std::get<pb::AppendEntriesRequest>(codec(1).encode_request(append()));
    if (level == 0) unknown(message);
    if (level == 1) unknown(*message.mutable_context());
    if (level == 2) unknown(*message.mutable_entries(0));
    EXPECT_THROW(codec(2).decode_request(message),std::invalid_argument);
  }
  auto reply = std::get<pb::RequestVoteResponse>(codec(2).encode_response({2,1,VoteResponse{3,true}},vote()));
  unknown(*reply.mutable_context()); EXPECT_THROW(codec(1).decode_response(reply,vote()),std::invalid_argument);
}
TEST(RaftWire, RejectsNoncontiguousIndicesAndInvalidEntryTerms) {
  const auto original = std::get<pb::AppendEntriesRequest>(codec(1).encode_request(append()));
  for (int change = 0; change < 7; ++change) {
    auto message = original;
    switch (change) {
      case 0: message.mutable_entries(0)->set_index(1); break;
      case 1: message.mutable_entries(1)->set_index(4); break;
      case 2: message.mutable_entries(0)->set_term(0); break;
      case 3: message.mutable_entries(1)->set_term(1); break;
      case 4: message.mutable_entries(1)->set_term(4); break;
      case 5: message.set_prev_log_index(UINT64_MAX); break;
      case 6: message.set_leader_commit(UINT64_MAX); break;
    }
    EXPECT_THROW(codec(2).decode_request(message),std::invalid_argument);
  }
}
TEST(RaftWire, RejectsImpossibleVotePositionsAndOutboundRoutes) {
  for (const auto invalid : {VoteRequest{0,0,0},VoteRequest{3,0,1},VoteRequest{3,1,0},
      VoteRequest{3,1,4},VoteRequest{3,kMaxLogEntries+1,3}}) {
    EXPECT_THROW(codec(1).encode_request({1,2,invalid}),std::invalid_argument);
    auto message = std::get<pb::RequestVoteRequest>(codec(1).encode_request(vote()));
    message.set_term(invalid.term); message.mutable_context()->set_request_term(invalid.term);
    message.set_last_log_index(invalid.last_index); message.set_last_log_term(invalid.last_term);
    EXPECT_THROW(codec(2).decode_request(message),std::invalid_argument);
  }
  for (const auto& invalid : {Envelope{2,1,VoteRequest{1,0,0}},Envelope{1,1,VoteRequest{1,0,0}},
      Envelope{1,6,VoteRequest{1,0,0}},Envelope{1,2,VoteResponse{1,true}}}) {
    EXPECT_THROW(codec(1).encode_request(invalid),std::invalid_argument);
  }
}
TEST(RaftWire, BoundsCountsCommandsAndAggregatePayloadBeforeCoreDelivery) {
  auto call = append(); auto& request = std::get<AppendRequest>(call.message);
  request.entries.assign(kMaxAppendEntries,{3,""});
  EXPECT_NO_THROW(codec(2).decode_request(codec(1).encode_request(call)));
  request.entries.push_back({3,""}); EXPECT_THROW(codec(1).encode_request(call),std::invalid_argument);
  request.entries = {{3,std::string(kMaxCommandBytes,'x')}};
  EXPECT_NO_THROW(codec(2).decode_request(codec(1).encode_request(call)));
  request.entries[0].command.push_back('x'); EXPECT_THROW(codec(1).encode_request(call),std::invalid_argument);
  request.entries.assign(4,{3,std::string(kMaxCommandBytes,'x')});
  EXPECT_THROW(codec(1).encode_request(call),std::invalid_argument);
  request.entries.back().command.resize(kMaxAppendBytes-3*kMaxCommandBytes);
  const auto maximum = serialize_raft_wire(codec(1).encode_request(call));
  EXPECT_LE(maximum.size(),kMaxAppendWireBytes);
  EXPECT_NO_THROW(codec(2).parse_request(WireMethod::append,maximum));
  request.entries.back().command.push_back('x');
  EXPECT_THROW(codec(1).encode_request(call),std::invalid_argument);
  auto wire = std::get<pb::AppendEntriesRequest>(codec(1).encode_request(append()));
  while (wire.entries_size() <= static_cast<int>(kMaxAppendEntries)) *wire.add_entries() = wire.entries(0);
  EXPECT_THROW(codec(2).decode_request(wire),std::invalid_argument);
  wire = std::get<pb::AppendEntriesRequest>(codec(1).encode_request(append()));
  wire.mutable_entries(0)->set_command(std::string(kMaxCommandBytes+1,'x'));
  EXPECT_THROW(codec(2).decode_request(wire),std::invalid_argument);
}
TEST(RaftWire, EmptyAndMalformedRawBuffersAndUnsupportedMethodsFail) {
  for (const auto& bytes : {std::string{},std::string(1,static_cast<char>(0xff)),std::string(kMaxAppendWireBytes+1,'x')}) {
    EXPECT_THROW(codec(2).parse_request(WireMethod::append,bytes),std::invalid_argument);
    EXPECT_THROW(codec(1).parse_response(bytes,append()),std::invalid_argument);
  }
  EXPECT_THROW(codec(2).parse_request(static_cast<WireMethod>(99),"x"),std::invalid_argument);
}
TEST(RaftWire, ResponseMustMatchOriginalPeerTermAndRpc) {
  const auto original = std::get<pb::AppendEntriesResponse>(codec(2).encode_response({2,1,AppendResponse{3,42,true,3,0}},append()));
  for (int change = 0; change < 7; ++change) {
    auto message = original;
    switch (change) {
      case 0: message.mutable_context()->set_sender_id("n3"); break;
      case 1: message.mutable_context()->set_recipient_id("n3"); break;
      case 2: message.mutable_context()->set_request_term(2); break;
      case 3: message.mutable_context()->set_rpc_id(41); break;
      case 4: message.set_term(2); break;
      case 5: message.set_match_index(4); break;
      case 6: message.set_conflict_term(2); break;
    }
    EXPECT_THROW(codec(1).decode_response(message,append()),std::invalid_argument);
  }
  auto next = append(); ++std::get<AppendRequest>(next.message).rpc;
  EXPECT_THROW(codec(1).decode_response(original,next),std::invalid_argument);
  EXPECT_THROW(codec(1).decode_response(original,vote()),std::invalid_argument);
}
TEST(RaftWire, HigherTermRejectionIsDeliveredButHigherTermSuccessIsInvalid) {
  const auto call = append(); const auto leader = codec(1), follower = codec(2);
  EXPECT_NO_THROW(leader.decode_response(follower.encode_response({2,1,AppendResponse{4,42,false,0,4097}},call),call));
  EXPECT_THROW(follower.encode_response({2,1,AppendResponse{4,42,true,3,0}},call),std::invalid_argument);
  EXPECT_THROW(follower.encode_response({2,1,AppendResponse{3,42,false,0,2}},call),std::invalid_argument);
  EXPECT_THROW(follower.encode_response({2,1,VoteResponse{4,true}},vote()),std::invalid_argument);
  EXPECT_NO_THROW(leader.decode_response(follower.encode_response({2,1,VoteResponse{4,false}},vote()),vote()));
}
TEST(RaftWire, InvalidTrafficCannotChangePersistentCoreState) {
  State state; unsigned writes = 0;
  auto config = raft_config(); config.id = 2;
  Core core(config,state,[&](const State& next) { state = next; ++writes; });
  auto message = std::get<pb::AppendEntriesRequest>(codec(1).encode_request(append()));
  message.set_term(UINT64_MAX); message.mutable_context()->set_request_term(UINT64_MAX);
  message.mutable_entries(0)->set_index(100);
  EXPECT_THROW(core.receive(codec(2).decode_request(message),0),std::invalid_argument);
  EXPECT_EQ(writes,0U); EXPECT_EQ(state.term,0U); EXPECT_FALSE(core.failed());
}
// Actual generated protobuf bytes on every link, with deterministic time and
// memory-backed durable images. This is deliberately not a socket/disk test.
class WireCluster {
 public:
  struct Replica {
    explicit Replica(NodeId id) : wire(codec(id)), identity_record(encode_manifest(wire.manifest())),
      applied{0,ledger_genesis().accounts} {}
    WireCodec wire;
    std::string identity_record;
    State disk;
    LedgerState applied;
    std::map<Hash,LedgerReceipt> receipts;
    std::unique_ptr<Core> core;
  };
  struct Packet { NodeId from, to; Envelope call; bool response; std::string bytes; };
  std::map<NodeId,std::unique_ptr<Replica>> nodes;
  std::deque<Packet> queue;
  std::set<NodeId> minority;
  Millis now = 0;
  WireCluster() {
    for (NodeId id = 1; id <= 5; ++id) { nodes[id] = std::make_unique<Replica>(id); restart(id); }
  }
  void restart(NodeId id) {
    auto& node = *nodes.at(id); auto config = raft_config(); config.id = id; config.seed = id*991;
    node.wire = WireCodec(decode_manifest(node.identity_record));
    if (node.wire.manifest().node_id != config.id || node.wire.manifest().members != config.members)
      throw std::runtime_error("runtime identity does not match persisted membership");
    node.core = std::make_unique<Core>(config,node.disk,[&,id](const State& next) {
      const auto& manifest = nodes.at(id)->wire.manifest();
      validate_persisted_transition(manifest,nodes.at(id)->disk,next);
      nodes.at(id)->disk = decode_state_record(manifest,encode_state_record(manifest,next));
    },now);
  }
  void emit(const Envelope& message, const std::optional<Envelope>& call = {}) {
    auto& wire = nodes.at(message.from)->wire;
    const bool response = std::holds_alternative<VoteResponse>(message.message) || std::holds_alternative<AppendResponse>(message.message);
    if (response) {
      if (!call) throw std::runtime_error("response has no originating call");
      queue.push_back({message.from,message.to,*call,true,serialize_raft_wire(wire.encode_response(message,*call))});
    } else queue.push_back({message.from,message.to,message,false,serialize_raft_wire(wire.encode_request(message))});
  }
  void apply(Replica& node) {
    if (node.applied.applied_index == node.disk.commit) return;
    const ReceiptLookup lookup = [&](const Hash& id) -> std::optional<LedgerReceipt> {
      const auto found = node.receipts.find(id);
      return found == node.receipts.end() ? std::nullopt : std::optional{found->second};
    };
    while (node.applied.applied_index < node.disk.commit) {
      const auto plan = plan_committed_entry(ledger_genesis(),node.applied,node.disk,node.applied.applied_index+1,lookup);
      for (const auto& receipt : plan.new_receipts) node.receipts.emplace(receipt.transaction_id,receipt);
      node.applied = plan.state;
    }
    audit_raft_application(ledger_genesis(),node.applied,node.disk,lookup);
  }
  void drain() {
    std::size_t budget = 10000;
    while (!queue.empty()) {
      if (budget-- == 0) throw std::runtime_error("wire simulation failed to quiesce");
      auto packet = std::move(queue.front()); queue.pop_front(); auto& receiver = *nodes.at(packet.to);
      if (!receiver.core || minority.contains(packet.from) != minority.contains(packet.to)) continue;
      const auto message = packet.response ? receiver.wire.parse_response(packet.bytes,packet.call) :
        receiver.wire.parse_request(std::holds_alternative<VoteRequest>(packet.call.message) ? WireMethod::vote : WireMethod::append,packet.bytes);
      const auto effects = receiver.core->receive(message,now);
      apply(receiver);
      for (const auto& effect : effects) emit(effect,message);
      // Invariant includes crashed replicas' retained committed prefixes.
      for (const auto& [a, first] : nodes) for (const auto& [b, second] : nodes) {
        (void)a; (void)b;
        for (Index index = 0; index < std::min(first->disk.commit,second->disk.commit); ++index)
          if (first->disk.log[index] != second->disk.log[index]) throw std::runtime_error("conflicting committed prefix");
      }
    }
  }
  void advance(Millis duration) {
    const auto end = now+duration;
    while (now < end) {
      now += 10;
      for (auto& [id,node] : nodes) {
        (void)id; if (!node->core) continue;
        for (const auto& effect : node->core->tick(now)) emit(effect);
        apply(*node);
      }
      drain();
    }
  }
  NodeId leader() const {
    for (const auto& [id,node] : nodes)
      if (!minority.contains(id) && node->core && node->core->role() == Role::leader) return id;
    throw std::runtime_error("no majority leader before deadline");
  }
  void propose(std::vector<SignedTransaction> transactions) {
    auto& node = *nodes.at(leader());
    for (const auto& effect : node.core->propose(encode_transfer_command(transactions),now)) emit(effect);
    drain(); advance(100);
  }
};
TEST(RaftWireCluster, FiveReplicasApplySignedTransfersAndRetryAfterLeaderRestart) {
  WireCluster cluster; cluster.advance(1200); cluster.propose({ledger_transaction()});
  const auto old = cluster.leader(); cluster.nodes.at(old)->core.reset();
  cluster.advance(1200); ASSERT_NE(cluster.leader(),old);
  cluster.propose({ledger_transaction(),ledger_transaction(1,20)});
  cluster.restart(old); cluster.advance(400);
  for (const auto& [id,node] : cluster.nodes) {
    SCOPED_TRACE(id); EXPECT_EQ(node->applied.accounts[0].balance,70U);
    EXPECT_EQ(node->applied.accounts[0].nonce,2U); EXPECT_EQ(node->receipts.size(),2U);
    EXPECT_EQ(node->applied.applied_index,node->disk.commit);
  }
}
TEST(RaftWireCluster, IsolatedLeaderCannotCommitAndReplicasConvergeAfterHealing) {
  WireCluster cluster; cluster.advance(1200); cluster.propose({ledger_transaction()});
  const auto old = cluster.leader(); const auto other = old == 1 ? 2U : 1U;
  cluster.minority = {old,other}; const auto committed = cluster.nodes.at(old)->disk.commit;
  auto& isolated = *cluster.nodes.at(old);
  for (const auto& effect : isolated.core->propose(encode_transfer_command({ledger_transaction(1,30)}),cluster.now)) cluster.emit(effect);
  cluster.drain(); cluster.advance(1400);
  EXPECT_EQ(isolated.disk.commit,committed); ASSERT_NE(cluster.leader(),old);
  cluster.propose({ledger_transaction(1,20)}); cluster.minority.clear(); cluster.advance(500);
  for (const auto& [id,node] : cluster.nodes) {
    SCOPED_TRACE(id); EXPECT_EQ(node->applied.accounts[0].balance,70U);
    EXPECT_EQ(node->applied.accounts[0].nonce,2U); EXPECT_EQ(node->receipts.size(),2U);
    EXPECT_FALSE(node->receipts.contains(ledger_transaction(1,30).id));
  }
}
} // namespace
