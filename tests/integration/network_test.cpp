#include "../network_support.hpp"
#include "../test_support.hpp"
#include "rollupx/node.hpp"
#include "consensus.grpc.pb.h"
#include "ledger.grpc.pb.h"
#include <arpa/inet.h>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <thread>

namespace {
using namespace std::chrono_literals;
using rollupx::test::Ports;
bool ready(const std::string& endpoint, int expected_peers) {
  auto client = rollupx::v1::Ledger::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now() + 200ms);
  rollupx::v1::ClusterStatus status;
  const auto result = client->GetClusterStatus(&context, rollupx::v1::Empty{}, &status);
  return result.ok() && status.stage() == rollupx::v1::ClusterStatus::NETWORKING_ONLY &&
    status.role() == rollupx::v1::ClusterStatus::ROLE_UNSPECIFIED && status.leader_id().empty() &&
    status.term() == 0 && status.commit_index() == 0 && status.applied_index() == 0 &&
    status.peers_size() == expected_peers && status.transport_ready();
}
template<class Predicate> bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(20ms); // Poll observable state, bounded by explicit deadline.
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
class Mesh : public ::testing::Test {
 protected:
  void SetUp() override {
    Ports ports;
    for (int i = 0; i < 5; ++i) {
      rollupx::Config c; c.id = "n" + std::to_string(i + 1);
      c.data_dir = directory.path + "/" + c.id;
      c.public_listen = ports.next(); c.internal_listen = ports.next(); c.http_listen = ports.next();
      c.probe_interval = 30ms; c.rpc_timeout = 100ms; c.peer_stale_after = 300ms;
      configs.push_back(c);
    }
    for (auto& c : configs) for (const auto& other : configs) c.peers.push_back({other.id, other.internal_listen});
    ports.release();
    for (const auto& c : configs) nodes.push_back(std::make_unique<rollupx::Node>(c));
    ASSERT_TRUE(eventually([&] { return all_ready(); }));
  }
  bool all_ready() {
    for (const auto& c : configs) {
      if (!ready(c.public_listen, 4)) {
        return false;
      }
    }
    return true;
  }
  rollupx::test::Directory directory;
  std::vector<rollupx::Config> configs;
  std::vector<std::unique_ptr<rollupx::Node>> nodes;
};
TEST_F(Mesh, FiveNodesExchangeMessagesAndRecoverConnectivityAfterRestart) {
  nodes[2].reset();
  ASSERT_TRUE(eventually([&] { return !ready(configs[0].public_listen, 4); }));
  nodes[2] = std::make_unique<rollupx::Node>(configs[2]);
  EXPECT_TRUE(eventually([&] { return all_ready(); }));
}
TEST_F(Mesh, RejectsWrongClusterUnknownSenderAndTopology) {
  auto stub = rollupx::internal::v1::PeerTransport::NewStub(
    grpc::CreateChannel(configs[0].internal_listen, grpc::InsecureChannelCredentials()));
  rollupx::internal::v1::ProbeRequest request;
  request.set_cluster_id("wrong"); request.set_sender_id("n2");
  request.set_protocol_version(1); request.set_topology(configs[0].topology());
  const auto invoke = [&] {
    grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now() + 500ms);
    rollupx::internal::v1::ProbeResponse response;
    return stub->Probe(&context, request, &response).error_code();
  };
  EXPECT_EQ(invoke(), grpc::StatusCode::FAILED_PRECONDITION);
  request.set_cluster_id(configs[0].cluster_id); request.set_sender_id("unknown");
  EXPECT_EQ(invoke(), grpc::StatusCode::PERMISSION_DENIED);
  request.set_sender_id("n1"); EXPECT_EQ(invoke(), grpc::StatusCode::PERMISSION_DENIED);
  request.set_sender_id("n2"); request.set_topology("different");
  EXPECT_EQ(invoke(), grpc::StatusCode::FAILED_PRECONDITION);
  request.set_topology(configs[0].topology()); request.set_protocol_version(2);
  EXPECT_EQ(invoke(), grpc::StatusCode::FAILED_PRECONDITION);
}
TEST_F(Mesh, DoesNotFakeConsensusOrTransactionSuccess) {
  auto stub = rollupx::v1::Ledger::NewStub(grpc::CreateChannel(configs[0].public_listen, grpc::InsecureChannelCredentials()));
  grpc::ClientContext transaction_context;
  transaction_context.set_deadline(std::chrono::system_clock::now() + 500ms);
  rollupx::v1::Receipt receipt;
  EXPECT_EQ(stub->SubmitTransaction(&transaction_context, rollupx::v1::Transaction{}, &receipt).error_code(), grpc::StatusCode::UNIMPLEMENTED);
  grpc::ClientContext leader_context; leader_context.set_deadline(std::chrono::system_clock::now() + 500ms);
  rollupx::v1::LeaderResponse leader;
  EXPECT_EQ(stub->GetLeader(&leader_context, rollupx::v1::Empty{}, &leader).error_code(), grpc::StatusCode::UNAVAILABLE);
  auto raft = rollupx::internal::v1::Raft::NewStub(grpc::CreateChannel(configs[0].internal_listen, grpc::InsecureChannelCredentials()));
  grpc::ClientContext vote_context; vote_context.set_deadline(std::chrono::system_clock::now() + 500ms);
  rollupx::internal::v1::RequestVoteResponse vote;
  EXPECT_EQ(raft->RequestVote(&vote_context, rollupx::internal::v1::RequestVoteRequest{}, &vote).error_code(), grpc::StatusCode::UNIMPLEMENTED);
  auto wrong_listener = rollupx::internal::v1::PeerTransport::NewStub(grpc::CreateChannel(configs[0].public_listen, grpc::InsecureChannelCredentials()));
  grpc::ClientContext probe_context; probe_context.set_deadline(std::chrono::system_clock::now() + 500ms);
  rollupx::internal::v1::ProbeResponse probe;
  EXPECT_EQ(wrong_listener->Probe(&probe_context, rollupx::internal::v1::ProbeRequest{}, &probe).error_code(), grpc::StatusCode::UNIMPLEMENTED);
}
} // namespace
