#include "rollupx/node.hpp"
#include "rollupx/http_server.hpp"
#include "rollupx/identity_store.hpp"
#include "consensus.grpc.pb.h"
#include "ledger.grpc.pb.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace rollupx {
namespace peer = internal::v1;
using Clock = std::chrono::steady_clock;
namespace {
constexpr int kMaxRpcBytes = 1024 * 1024;
constexpr std::uint32_t kProtocolVersion = 1;
std::unique_ptr<grpc::Server> listen(const std::string& address, grpc::Service* first,
                                     grpc::Service* second = nullptr) {
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(kMaxRpcBytes);
  builder.SetMaxSendMessageSize(kMaxRpcBytes);
  grpc::ResourceQuota quota;
  quota.Resize(64 * 1024 * 1024);
  quota.SetMaxThreads(16);
  builder.SetResourceQuota(quota);
  // Do not allow another node to silently share the listener via SO_REUSEPORT.
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, 1);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 1);
  builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 4);
  int selected_port = 0;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &selected_port);
  builder.RegisterService(first);
  if (second) builder.RegisterService(second);
  auto server = builder.BuildAndStart();
  if (!server || selected_port == 0) throw std::runtime_error("cannot bind gRPC listener " + address);
  return server;
}
} // namespace
struct Node::Impl {
  struct PeerState {
    Peer member;
    Clock::time_point last_success{};
    std::uint64_t successes = 0;
    std::uint64_t failures = 0;
  };
  struct Transport final : peer::PeerTransport::Service {
    explicit Transport(Impl& owner) : owner(owner) {}
    grpc::Status Probe(grpc::ServerContext*, const peer::ProbeRequest* request,
                       peer::ProbeResponse* response) override {
      if (!owner.running.load()) return {grpc::StatusCode::UNAVAILABLE, "node starting or stopping"};
      if (request->cluster_id() != owner.config.cluster_id ||
          request->protocol_version() != kProtocolVersion || request->topology() != owner.config.topology())
        return {grpc::StatusCode::FAILED_PRECONDITION, "cluster, topology or protocol mismatch"};
      const bool member = std::any_of(owner.config.peers.begin(), owner.config.peers.end(), [&](const auto& item) {
        return item.id == request->sender_id() && item.id != owner.config.id;
      });
      if (!member) return {grpc::StatusCode::PERMISSION_DENIED, "sender is not another configured member"};
      owner.received.fetch_add(1);
      response->set_node_id(owner.config.id);
      response->set_cluster_id(owner.config.cluster_id);
      response->set_protocol_version(kProtocolVersion);
      return grpc::Status::OK;
    }
    Impl& owner;
  };
  struct Ledger final : v1::Ledger::Service {
    explicit Ledger(Impl& owner) : owner(owner) {}
    grpc::Status GetClusterStatus(grpc::ServerContext*, const v1::Empty*, v1::ClusterStatus* response) override {
      if (!owner.running.load()) return {grpc::StatusCode::UNAVAILABLE, "node starting or stopping"};
      *response = owner.status();
      return grpc::Status::OK;
    }
    grpc::Status GetLeader(grpc::ServerContext*, const v1::Empty*, v1::LeaderResponse*) override {
      return {grpc::StatusCode::UNAVAILABLE, "Raft not implemented; networking-only milestone"};
    }
    // Generated defaults return UNIMPLEMENTED for all transaction/ledger methods.
    Impl& owner;
  };

  explicit Impl(Config value) : config(std::move(value)), store(config), transport(*this), ledger(*this) {
    for (const auto& member : config.peers) if (member.id != config.id) peers.push_back({member});
  }
  ~Impl() { stop(); }
  void start() {
    internal_server = listen(config.internal_listen, &transport, &raft);
    public_server = listen(config.public_listen, &ledger);
    http = std::make_unique<HttpServer>(parse_endpoint(config.http_listen), [this](const std::string& path) {
      if (path == "/healthz") return HttpResponse{running.load() && !failed.load() ? 200 : 503,
        "text/plain", running.load() && !failed.load() ? "alive\n" : "unavailable\n"};
      if (path == "/readyz") {
        const bool ready = running.load() && !failed.load() && status().transport_ready();
        return HttpResponse{ready ? 200 : 503, "text/plain", ready ? "transport ready (no consensus)\n" : "transport not ready\n"};
      }
      if (path == "/metrics") return HttpResponse{200, "text/plain; version=0.0.4", metrics()};
      return HttpResponse{404, "text/plain", "not found\n"};
    });
    running.store(true);
    for (std::size_t index = 0; index < peers.size(); ++index) {
      workers.emplace_back([this, index](std::stop_token token) {
        try { probe_loop(index, token); }
        catch (const std::exception& error) {
          std::cerr << "peer worker fatal: " << error.what() << '\n'; failed.store(true);
        } catch (...) { std::cerr << "peer worker fatal: unknown exception\n"; failed.store(true); }
      });
    }
  }
  void stop() {
    running.store(false);
    for (auto& worker : workers) worker.request_stop();
    wake.notify_all();
    workers.clear(); // Joins; each in-flight RPC has a bounded deadline.
    if (http) http->stop();
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
    if (public_server) { public_server->Shutdown(deadline); public_server->Wait(); public_server.reset(); }
    if (internal_server) { internal_server->Shutdown(deadline); internal_server->Wait(); internal_server.reset(); }
  }
  v1::ClusterStatus status() const {
    v1::ClusterStatus result;
    result.set_node_id(config.id);
    result.set_cluster_id(config.cluster_id);
    result.set_stage(v1::ClusterStatus::NETWORKING_ONLY);
    bool ready = running.load() && !failed.load();
    const auto now = Clock::now();
    std::lock_guard lock(mutex);
    for (const auto& state : peers) {
      auto* item = result.add_peers();
      item->set_node_id(state.member.id); item->set_endpoint(state.member.endpoint);
      const bool reachable = state.successes > 0 && now - state.last_success <= config.peer_stale_after;
      item->set_reachable(reachable);
      item->set_successful_probes(state.successes); item->set_failed_probes(state.failures);
      ready = ready && reachable;
    }
    result.set_transport_ready(ready);
    return result;
  }
  std::string metrics() const {
    const auto snapshot = status();
    std::ostringstream out;
    out << "# HELP rollupx_consensus_enabled Whether this binary implements consensus.\n"
        << "# TYPE rollupx_consensus_enabled gauge\nrollupx_consensus_enabled 0\n"
        << "# HELP rollupx_transport_ready All peers recently reachable; not ledger readiness.\n"
        << "# TYPE rollupx_transport_ready gauge\nrollupx_transport_ready " << snapshot.transport_ready() << '\n'
        << "# TYPE rollupx_peer_probes_received_total counter\nrollupx_peer_probes_received_total " << received.load() << '\n'
        << "# TYPE rollupx_peer_reachable gauge\n"
        << "# TYPE rollupx_peer_probes_total counter\n";
    for (const auto& item : snapshot.peers()) {
      out << "rollupx_peer_reachable{peer=\"" << item.node_id() << "\"} " << item.reachable() << '\n'
          << "rollupx_peer_probes_total{peer=\"" << item.node_id() << "\",result=\"success\"} " << item.successful_probes() << '\n'
          << "rollupx_peer_probes_total{peer=\"" << item.node_id() << "\",result=\"failure\"} " << item.failed_probes() << '\n';
    }
    return out.str();
  }
  void probe_loop(std::size_t index, std::stop_token token) {
    const auto member = peers.at(index).member; // Topology is immutable after construction.
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(kMaxRpcBytes);
    args.SetMaxSendMessageSize(kMaxRpcBytes);
    auto stub = peer::PeerTransport::NewStub(grpc::CreateCustomChannel(member.endpoint, grpc::InsecureChannelCredentials(), args));
    peer::ProbeRequest request;
    request.set_cluster_id(config.cluster_id); request.set_sender_id(config.id);
    request.set_protocol_version(kProtocolVersion); request.set_topology(config.topology());
    while (!token.stop_requested()) {
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + config.rpc_timeout);
      peer::ProbeResponse response;
      const auto call = stub->Probe(&context, request, &response);
      const bool success = call.ok() && response.node_id() == member.id &&
        response.cluster_id() == config.cluster_id && response.protocol_version() == kProtocolVersion;
      {
        std::lock_guard lock(mutex);
        auto& state = peers.at(index);
        if (success) { ++state.successes; state.last_success = Clock::now(); }
        else ++state.failures;
      }
      std::unique_lock lock(wait_mutex);
      wake.wait_for(lock, token, config.probe_interval, [] { return false; });
    }
  }

  const Config config;
  IdentityStore store;
  mutable std::mutex mutex;
  std::vector<PeerState> peers;
  std::atomic<bool> running{false}, failed{false};
  std::atomic<std::uint64_t> received{0};
  Transport transport;
  Ledger ledger;
  peer::Raft::Service raft;
  std::unique_ptr<grpc::Server> internal_server, public_server;
  std::unique_ptr<HttpServer> http;
  std::mutex wait_mutex;
  std::condition_variable_any wake;
  std::vector<std::jthread> workers;
};
Node::Node(Config config) : impl_(std::make_unique<Impl>(std::move(config))) { impl_->start(); }
Node::~Node() = default;
void Node::stop() { impl_->stop(); }
bool Node::healthy() const { return impl_->running.load() && !impl_->failed.load() && impl_->http && impl_->http->healthy(); }
} // namespace rollupx
