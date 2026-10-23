#include "rollupx/raft_grpc.hpp"
#include "rollupx/config.hpp"
#include <algorithm>
#include <type_traits>

namespace rollupx::raft {
namespace {
RpcCode rpc_code(const grpc::Status& status) {
  if (status.ok()) return RpcCode::ok;
  switch (status.error_code()) {
    case grpc::StatusCode::CANCELLED: return RpcCode::cancelled;
    case grpc::StatusCode::DEADLINE_EXCEEDED: return RpcCode::deadline;
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return RpcCode::exhausted;
    case grpc::StatusCode::INVALID_ARGUMENT: return RpcCode::invalid;
    case grpc::StatusCode::UNAVAILABLE: return RpcCode::unavailable;
    default: return RpcCode::failed;
  }
}
grpc::Status grpc_status(RpcCode code) {
  switch (code) {
    case RpcCode::ok: return grpc::Status::OK;
    case RpcCode::cancelled: return {grpc::StatusCode::CANCELLED,"Raft call cancelled; execution may have started"};
    case RpcCode::deadline: return {grpc::StatusCode::DEADLINE_EXCEEDED,"Raft call expired; execution may have started"};
    case RpcCode::exhausted: return {grpc::StatusCode::RESOURCE_EXHAUSTED,"Raft admission capacity exhausted"};
    case RpcCode::invalid: return {grpc::StatusCode::INVALID_ARGUMENT,"invalid Raft message"};
    case RpcCode::unavailable: return {grpc::StatusCode::UNAVAILABLE,"Raft runtime unavailable"};
    case RpcCode::failed: return {grpc::StatusCode::INTERNAL,"Raft processing failed"};
  }
  return {grpc::StatusCode::INTERNAL,"unknown Raft outcome"};
}
template<class Request, class Response>
grpc::Status serve(RaftIngress& ingress, grpc::ServerContext* context, const Request& request, Response& response) {
  try {
    const auto now = std::chrono::system_clock::now();
    if (context->deadline() <= now) return grpc_status(RpcCode::deadline);
    const auto budget = std::min(context->deadline(),now+std::chrono::seconds(10))-now;
    auto result = ingress.respond(request,std::chrono::steady_clock::now()+budget,[&] { return context->IsCancelled(); });
    if (result.code != RpcCode::ok) return grpc_status(result.code);
    if (!result.response || !std::holds_alternative<Response>(*result.response)) return grpc_status(RpcCode::failed);
    response = std::move(std::get<Response>(*result.response)); return grpc::Status::OK;
  } catch (...) { return grpc_status(RpcCode::failed); }
}
} // namespace
GrpcRaftClient::GrpcRaftClient(const Manifest& identity, const std::map<NodeId,std::string>& endpoints) {
  (void)configuration_digest(identity); // Named identities are mandatory.
  if (endpoints.size()+1 != identity.members.size()) throw std::invalid_argument("incomplete Raft endpoint mapping");
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(static_cast<int>(kMaxVoteWireBytes));
  arguments.SetMaxSendMessageSize(static_cast<int>(kMaxAppendWireBytes));
  for (const auto id : identity.members) if (id != identity.node_id) {
    const auto found = endpoints.find(id);
    if (found == endpoints.end()) throw std::invalid_argument("missing Raft peer endpoint");
    (void)rollupx::parse_endpoint(found->second);
    peers_.emplace(id,internal::v1::Raft::NewStub(grpc::CreateCustomChannel(found->second,grpc::InsecureChannelCredentials(),arguments)));
  }
}
RpcResult GrpcRaftClient::call(NodeId peer, const WireRequest& request, RpcDeadline deadline, std::stop_token stop) const {
  const auto found = peers_.find(peer);
  if (found == peers_.end()) throw std::invalid_argument("unconfigured gRPC peer");
  if (stop.stop_requested()) return {RpcCode::cancelled};
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return {RpcCode::deadline};
  grpc::ClientContext context;
  const auto budget = std::min(deadline-now,
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(10)));
  context.set_deadline(std::chrono::system_clock::now()+
    std::chrono::duration_cast<std::chrono::system_clock::duration>(budget));
  std::stop_callback cancellation(stop,[&] { context.TryCancel(); });
  return std::visit([&](const auto& message) -> RpcResult {
    using T = std::decay_t<decltype(message)>;
    if constexpr (std::is_same_v<T,internal::v1::RequestVoteRequest>) {
      internal::v1::RequestVoteResponse response;
      const auto status = found->second->RequestVote(&context,message,&response);
      return status.ok() ? RpcResult{RpcCode::ok,WireResponse{std::move(response)}} : RpcResult{rpc_code(status)};
    } else {
      internal::v1::AppendEntriesResponse response;
      const auto status = found->second->AppendEntries(&context,message,&response);
      return status.ok() ? RpcResult{RpcCode::ok,WireResponse{std::move(response)}} : RpcResult{rpc_code(status)};
    }
  },request);
}
grpc::Status GrpcRaftService::RequestVote(grpc::ServerContext* context, const internal::v1::RequestVoteRequest* request,
    internal::v1::RequestVoteResponse* response) { return serve(ingress_,context,*request,*response); }
grpc::Status GrpcRaftService::AppendEntries(grpc::ServerContext* context, const internal::v1::AppendEntriesRequest* request,
    internal::v1::AppendEntriesResponse* response) { return serve(ingress_,context,*request,*response); }
void configure_raft_server(grpc::ServerBuilder& builder) {
  builder.SetMaxReceiveMessageSize(static_cast<int>(kMaxAppendWireBytes));
  builder.SetMaxSendMessageSize(static_cast<int>(kMaxVoteWireBytes));
  grpc::ResourceQuota quota; quota.Resize(64*1024*1024); quota.SetMaxThreads(16); builder.SetResourceQuota(quota);
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT,0);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS,1);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS,1);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS,4);
}
} // namespace rollupx::raft
