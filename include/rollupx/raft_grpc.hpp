#pragma once
#include "rollupx/raft_transport.hpp"
#include "consensus.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <map>

namespace rollupx::raft {
// Private-development plaintext adapter. Endpoint keys must be exactly the remote
// numeric members derived from the named manifest. Object outlives dispatcher.
class GrpcRaftClient {
 public:
  GrpcRaftClient(const Manifest& identity, const std::map<NodeId,std::string>& endpoints);
  RpcResult call(NodeId peer, const WireRequest& request, RpcDeadline deadline, std::stop_token stop) const;
 private:
  std::map<NodeId,std::unique_ptr<internal::v1::Raft::Stub>> peers_;
};
class GrpcRaftService final : public internal::v1::Raft::Service {
 public:
  explicit GrpcRaftService(RaftIngress& ingress) : ingress_(ingress) {}
  grpc::Status RequestVote(grpc::ServerContext*, const internal::v1::RequestVoteRequest*, internal::v1::RequestVoteResponse*) override;
  grpc::Status AppendEntries(grpc::ServerContext*, const internal::v1::AppendEntriesRequest*, internal::v1::AppendEntriesResponse*) override;
  // InstallSnapshot retains the generated UNIMPLEMENTED behavior.
 private:
  RaftIngress& ingress_;
};
// Required server resource bounds for this synchronous service. The caller owns
// listeners, registration, shutdown, and Wait(); it must outlive active handlers.
void configure_raft_server(grpc::ServerBuilder& builder);
} // namespace rollupx::raft
