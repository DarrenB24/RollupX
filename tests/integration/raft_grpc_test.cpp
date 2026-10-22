#include "rollupx/raft_grpc.hpp"
#include <atomic>
#include <gtest/gtest.h>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace std::chrono_literals;
WireCodec identity(NodeId id) { Hash genesis{}; genesis[0] = 1; return WireCodec("grpc-test",{"n1","n2","n3"},genesis,id); }
Envelope vote() { return {1,2,VoteRequest{1,0,0}}; }
class GrpcTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    runtime = std::make_unique<Runtime>(Config{2,{1,2,3},50,300,600,2},State{},
      [&](const State&) { ++writes; },[&](Index index) { applied = index; return index; },
      [](const Envelope&) { return false; },RuntimeOptions{64,1024*1024,false,10ms});
    ingress = std::make_unique<RaftIngress>(identity(2),runtime->handle());
    service = std::make_unique<GrpcRaftService>(*ingress);
    grpc::ServerBuilder builder; configure_raft_server(builder);
    int port = 0; builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);
    builder.RegisterService(service.get()); server = builder.BuildAndStart();
    ASSERT_TRUE(server); ASSERT_GT(port,0);
    endpoint = "127.0.0.1:"+std::to_string(port);
    channel = grpc::CreateChannel(endpoint,grpc::InsecureChannelCredentials());
    ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now()+3s));
    stub = internal::v1::Raft::NewStub(channel);
    client = std::make_unique<GrpcRaftClient>(identity(1).manifest(),std::map<NodeId,std::string>{{2,endpoint},{3,endpoint}});
  }
  void TearDown() override {
    if (ingress) ingress->close();
    if (server) { server->Shutdown(std::chrono::system_clock::now()+2s); server->Wait(); server.reset(); }
    if (runtime) runtime->stop();
  }
  std::atomic<unsigned> writes{0}; std::atomic<Index> applied{0};
  std::unique_ptr<Runtime> runtime;
  std::unique_ptr<RaftIngress> ingress;
  std::unique_ptr<GrpcRaftService> service;
  std::unique_ptr<grpc::Server> server;
  std::string endpoint;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<internal::v1::Raft::Stub> stub;
  std::unique_ptr<GrpcRaftClient> client;
};
TEST_F(GrpcTransport, VoteAndCommittedAppendReachOwnerBeforeAcknowledgment) {
  auto result = client->call(2,identity(1).encode_request(vote()),std::chrono::steady_clock::now()+2s,{});
  ASSERT_EQ(result.code,RpcCode::ok); ASSERT_TRUE(result.response);
  EXPECT_TRUE(std::get<VoteResponse>(identity(1).decode_response(*result.response,vote()).message).granted);
  EXPECT_GT(writes.load(),0U);
  const Envelope append{1,2,AppendRequest{1,1,0,0,{{1,"command"}},1}};
  result = client->call(2,identity(1).encode_request(append),std::chrono::steady_clock::now()+2s,{});
  ASSERT_EQ(result.code,RpcCode::ok); ASSERT_TRUE(result.response);
  EXPECT_TRUE(std::get<AppendResponse>(identity(1).decode_response(*result.response,append).message).success);
  EXPECT_EQ(applied.load(),1U); EXPECT_EQ(runtime->status().commit,1U);
}
TEST_F(GrpcTransport, MalformedIdentityAndUnsupportedSnapshotCannotMutateState) {
  auto request = std::get<internal::v1::RequestVoteRequest>(identity(1).encode_request(vote()));
  request.mutable_context()->set_sender_id("unknown");
  grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+2s);
  internal::v1::RequestVoteResponse response;
  EXPECT_EQ(stub->RequestVote(&context,request,&response).error_code(),grpc::StatusCode::INVALID_ARGUMENT);
  grpc::ClientContext snapshot_context; snapshot_context.set_deadline(std::chrono::system_clock::now()+2s);
  internal::v1::InstallSnapshotRequest snapshot; internal::v1::InstallSnapshotResponse snapshot_response;
  EXPECT_EQ(stub->InstallSnapshot(&snapshot_context,snapshot,&snapshot_response).error_code(),grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(writes.load(),0U); EXPECT_EQ(runtime->status().term,0U);
}
TEST_F(GrpcTransport, ExpiredCancelledAndClosedCallsFailExplicitly) {
  const auto request = identity(1).encode_request(vote());
  EXPECT_EQ(client->call(2,request,std::chrono::steady_clock::now(),{}).code,RpcCode::deadline);
  std::stop_source cancellation; cancellation.request_stop();
  EXPECT_EQ(client->call(2,request,std::chrono::steady_clock::now()+2s,cancellation.get_token()).code,RpcCode::cancelled);
  ingress->close();
  EXPECT_EQ(client->call(2,request,std::chrono::steady_clock::now()+2s,{}).code,RpcCode::unavailable);
  EXPECT_EQ(writes.load(),0U);
}
TEST_F(GrpcTransport, EndpointMapMustCoverExactlyRemoteMembers) {
  EXPECT_THROW((GrpcRaftClient(identity(1).manifest(),{{2,endpoint}})),std::invalid_argument);
  EXPECT_THROW((GrpcRaftClient(identity(1).manifest(),{{1,endpoint},{2,endpoint}})),std::invalid_argument);
  EXPECT_THROW((GrpcRaftClient(identity(1).manifest(),{{2,"bad"},{3,endpoint}})),std::invalid_argument);
}
} // namespace
