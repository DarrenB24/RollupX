#pragma once
#include "rollupx/raft_runtime.hpp"
#include "rollupx/raft_wire.hpp"
#include <stop_token>

namespace rollupx::raft {
using RpcDeadline = std::chrono::steady_clock::time_point;
enum class RpcCode { ok, unavailable, deadline, cancelled, exhausted, invalid, failed };
struct RpcResult { RpcCode code; std::optional<WireResponse> response = {}; };
// Must honor deadline/stop and return only after it no longer uses request.
using PeerCall = std::function<RpcResult(NodeId, const WireRequest&, RpcDeadline, std::stop_token)>;
struct DispatcherStatus {
  std::size_t queued = 0, active = 0;
  std::uint64_t accepted = 0, backpressure = 0, rpc_errors = 0, invalid_replies = 0,
    delivered = 0, runtime_rejected = 0;
  bool bound = false, stopped = false, failed = false;
};
// One worker, one active call and one queued call per remote member. No sockets
// here: the production PeerCall adapter is GrpcRaftClient.
class PeerDispatcher {
 public:
  PeerDispatcher(WireCodec codec, PeerCall call, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
  ~PeerDispatcher();
  PeerDispatcher(const PeerDispatcher&) = delete;
  PeerDispatcher& operator=(const PeerDispatcher&) = delete;
  void bind(Runtime::Handle handle); // Exactly once, before making node ready.
  bool try_enqueue(const Envelope& request);
  DispatcherStatus status() const;
  std::exception_ptr failure() const;
  bool wait_idle(RpcDeadline deadline); // Includes response handoff, not application.
  void request_stop();
  void stop(); // Waits for cancellation-aware PeerCall adapters; cannot join itself.
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct IngressOptions {
  std::size_t max_calls = 16;
  std::chrono::milliseconds max_duration{500};
  std::chrono::milliseconds cancellation_poll{5};
};
struct IngressStatus { std::size_t active = 0; std::uint64_t accepted = 0, backpressure = 0; bool closed = false; };
// Callers must finish all respond calls before destroying this object.
class RaftIngress {
 public:
  RaftIngress(WireCodec codec, Runtime::Handle runtime, IngressOptions options = {});
  ~RaftIngress();
  RaftIngress(const RaftIngress&) = delete;
  RaftIngress& operator=(const RaftIngress&) = delete;
  RpcResult respond(const WireRequest& request, RpcDeadline deadline,
    const std::function<bool()>& cancelled = {});
  void close(); // Stops admission and cancels waits; active writes may finish.
  IngressStatus status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx::raft
