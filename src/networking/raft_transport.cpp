#include "rollupx/raft_transport.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rollupx::raft {
namespace { thread_local const void* active_dispatcher = nullptr; }
struct PeerDispatcher::Impl {
  struct Job { Envelope original; WireRequest wire; };
  struct Peer { NodeId id; std::optional<Job> pending; };
  Impl(WireCodec identity, PeerCall adapter, std::chrono::milliseconds duration)
    : codec(std::move(identity)), call(std::move(adapter)), timeout(duration) {
    if (!call || timeout.count() < 1 || timeout.count() > 10000) throw std::invalid_argument("invalid peer dispatcher options");
    for (const auto id : codec.manifest().members) if (id != codec.manifest().node_id) peers.push_back({id,{}});
  }
  ~Impl() { stop(); }
  void start() {
    try {
      for (std::size_t i = 0; i < peers.size(); ++i) workers.emplace_back([this,i] { run(i); });
    } catch (...) { stop(); throw; }
  }
  void halt() {
    {
      std::lock_guard lock(mutex); stats.stopped = true;
      for (auto& peer : peers) peer.pending.reset(); stats.queued = 0;
    }
    // Stop callbacks may acquire their own/our mutexes. Never invoke them locked.
    cancellation.request_stop(); wake.notify_all(); idle.notify_all();
  }
  void stop() {
    halt();
    if (active_dispatcher == this) throw std::logic_error("peer worker cannot join its dispatcher");
    std::lock_guard lock(join_mutex);
    for (auto& worker : workers) if (worker.joinable()) worker.join();
  }
  void run(std::size_t index) {
    active_dispatcher = this;
    for (;;) {
      std::optional<Job> job;
      {
        std::unique_lock lock(mutex); wake.wait(lock,[&] { return stats.stopped || peers[index].pending.has_value(); });
        if (stats.stopped) return;
        job = std::move(peers[index].pending); peers[index].pending.reset(); --stats.queued; ++stats.active;
      }
      bool fatal = false;
      try {
        const auto deadline = std::chrono::steady_clock::now()+timeout;
        auto result = call(peers[index].id,job->wire,deadline,cancellation.get_token());
        // Check local time too: a late backend success must not refresh quorum.
        if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline || result.code != RpcCode::ok) {
          std::lock_guard lock(mutex); ++stats.rpc_errors;
        } else if (!result.response) {
          std::lock_guard lock(mutex); ++stats.invalid_replies;
        } else {
          std::optional<Envelope> response;
          try { response = codec.decode_response(*result.response,job->original); }
          catch (const std::invalid_argument&) { std::lock_guard lock(mutex); ++stats.invalid_replies; }
          if (response) {
            // Serialize handoff with stop so no response is admitted after halt.
            std::lock_guard lock(mutex);
            if (std::chrono::steady_clock::now() >= deadline) ++stats.rpc_errors;
            else if (!stats.stopped) {
              const auto ticket = runtime.receive(std::move(*response));
              if (ticket.admission == Admission::queued) ++stats.delivered; else ++stats.runtime_rejected;
            }
          }
        }
      } catch (...) {
        { std::lock_guard lock(mutex); if (!error) error = std::current_exception(); stats.failed = true; }
        fatal = true; halt();
      }
      { std::lock_guard lock(mutex); --stats.active; idle.notify_all(); }
      if (fatal) return;
    }
  }
  WireCodec codec;
  PeerCall call;
  std::chrono::milliseconds timeout;
  std::vector<Peer> peers;
  std::vector<std::thread> workers;
  mutable std::mutex mutex;
  std::mutex join_mutex;
  std::condition_variable wake, idle;
  std::stop_source cancellation;
  DispatcherStatus stats;
  Runtime::Handle runtime;
  std::exception_ptr error;
};
PeerDispatcher::PeerDispatcher(WireCodec codec, PeerCall call, std::chrono::milliseconds timeout)
  : impl_(std::make_unique<Impl>(std::move(codec),std::move(call),timeout)) { impl_->start(); }
PeerDispatcher::~PeerDispatcher() = default;
void PeerDispatcher::bind(Runtime::Handle handle) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stats.bound || impl_->stats.stopped) throw std::logic_error("dispatcher cannot be rebound or restarted");
  impl_->runtime = std::move(handle); impl_->stats.bound = true;
}
bool PeerDispatcher::try_enqueue(const Envelope& request) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->error) std::rethrow_exception(impl_->error);
  if (!impl_->stats.bound || impl_->stats.stopped) return false;
  const auto peer = std::find_if(impl_->peers.begin(),impl_->peers.end(),[&](const auto& item) { return item.id == request.to; });
  if (peer == impl_->peers.end()) throw std::invalid_argument("unknown outbound Raft peer");
  if (peer->pending) { ++impl_->stats.backpressure; return false; }
  auto wire = impl_->codec.encode_request(request); // Bounds validation before copying original.
  peer->pending = Impl::Job{request,std::move(wire)}; ++impl_->stats.queued; ++impl_->stats.accepted;
  impl_->wake.notify_all(); return true;
}
DispatcherStatus PeerDispatcher::status() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
std::exception_ptr PeerDispatcher::failure() const { std::lock_guard lock(impl_->mutex); return impl_->error; }
bool PeerDispatcher::wait_idle(RpcDeadline deadline) {
  std::unique_lock lock(impl_->mutex);
  return impl_->idle.wait_until(lock,deadline,[&] { return impl_->stats.active == 0 && impl_->stats.queued == 0; });
}
void PeerDispatcher::request_stop() { impl_->halt(); }
void PeerDispatcher::stop() { impl_->stop(); }
struct RaftIngress::Impl {
  Impl(WireCodec identity, Runtime::Handle handle, IngressOptions value)
    : codec(std::move(identity)), runtime(std::move(handle)), options(value) {
    if (options.max_calls == 0 || options.max_calls > 64 || options.max_duration.count() < 1 || options.max_duration.count() > 10000 ||
        options.cancellation_poll.count() < 1 || options.cancellation_poll.count() > 50)
      throw std::invalid_argument("invalid Raft ingress options");
  }
  WireCodec codec; Runtime::Handle runtime; IngressOptions options;
  mutable std::mutex mutex; IngressStatus stats;
};
RaftIngress::RaftIngress(WireCodec codec, Runtime::Handle runtime, IngressOptions options)
  : impl_(std::make_unique<Impl>(std::move(codec),std::move(runtime),options)) {}
RaftIngress::~RaftIngress() = default;
RpcResult RaftIngress::respond(const WireRequest& request, RpcDeadline deadline, const std::function<bool()>& cancelled) {
  deadline = std::min(deadline,std::chrono::steady_clock::now()+impl_->options.max_duration);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stats.closed) return {RpcCode::unavailable};
    if (impl_->stats.active == impl_->options.max_calls) { ++impl_->stats.backpressure; return {RpcCode::exhausted}; }
    ++impl_->stats.active; ++impl_->stats.accepted;
  }
  struct Release {
    Impl& impl;
    ~Release() { std::lock_guard lock(impl.mutex); --impl.stats.active; }
  } release{*impl_};
  const auto interrupted = [&]() -> std::optional<RpcCode> {
    if (std::chrono::steady_clock::now() >= deadline) return RpcCode::deadline;
    if (cancelled && cancelled()) return RpcCode::cancelled;
    std::lock_guard lock(impl_->mutex);
    return impl_->stats.closed ? std::optional{RpcCode::cancelled} : std::nullopt;
  };
  if (const auto code = interrupted()) return {*code};
  Envelope decoded;
  try { decoded = impl_->codec.decode_request(request); }
  catch (const std::invalid_argument&) { return {RpcCode::invalid}; }
  if (const auto code = interrupted()) return {*code};
  auto ticket = impl_->runtime.receive(decoded);
  if (ticket.admission == Admission::full) return {RpcCode::exhausted};
  if (ticket.admission == Admission::invalid) return {RpcCode::invalid};
  if (ticket.admission != Admission::queued) return {RpcCode::unavailable};
  struct CancelOnExit { Ticket& ticket; ~CancelOnExit() { ticket.cancel(); } } cancel_on_exit{ticket};
  for (;;) {
    if (const auto code = interrupted()) { ticket.cancel(); return {*code}; }
    if (ticket.result.wait_until(std::min(deadline,std::chrono::steady_clock::now()+impl_->options.cancellation_poll)) != std::future_status::ready) continue;
    if (const auto code = interrupted()) { ticket.cancel(); return {*code}; }
    const auto result = ticket.result.get();
    if (result.completion == Completion::failed || result.status.failed) return {RpcCode::failed};
    if (result.completion != Completion::processed || result.status.stopped) return {RpcCode::unavailable};
    if (!result.reply) return {RpcCode::failed};
    return {RpcCode::ok,impl_->codec.encode_response(*result.reply,decoded)};
  }
}
void RaftIngress::close() { std::lock_guard lock(impl_->mutex); impl_->stats.closed = true; }
IngressStatus RaftIngress::status() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
} // namespace rollupx::raft
