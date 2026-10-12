#pragma once
#include "rollupx/raft.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace rollupx::raft {
enum class Admission { queued, full, invalid, stopped };
enum class Completion { processed, rejected, cancelled, stopped, failed };
struct RuntimeStatus {
  Role role = Role::follower;
  Term term = 0;
  std::optional<NodeId> leader;
  Index commit = 0, applied = 0;
  std::uint64_t elections = 0, outbound_dropped = 0, admission_rejected = 0;
  std::size_t pending_events = 0, pending_bytes = 0; // Includes active event.
  bool stopped = false, failed = false;
};
struct EventResult {
  Completion completion;
  std::optional<Envelope> reply; // Unary response, after persistence/application.
  std::optional<Index> proposed_index; // Admission to log, NOT confirmation.
  RuntimeStatus status;
};
struct Ticket {
  Admission admission = Admission::stopped;
  std::future<EventResult> result; // Valid only for queued admission.
  void cancel() const { if (cancellation) cancellation->store(true); }
  std::shared_ptr<std::atomic_bool> cancellation;
};
struct RuntimeOptions {
  std::size_t max_events = 256;
  std::size_t max_bytes = 8*1024*1024;
  bool automatic_ticks = true;
  std::chrono::milliseconds tick_interval{10};
};
using RuntimeClock = std::function<Millis()>;
// Called only by owner thread. Must return the exact durably applied commit index.
using ApplyCommitted = std::function<Index(Index)>;
// Nonblocking, bounded transport enqueue. False means packet loss/backpressure,
// counted in status; throw for a driver failure that must stop the node.
using Dispatch = std::function<bool(const Envelope&)>;
class Runtime {
  struct Inbox;
  struct Impl;
 public:
  class Handle {
   public:
    Handle() = default;
    Ticket receive(Envelope message) const;
    Ticket propose(std::string command) const;
    Ticket tick() const; // Explicit scheduler wake, useful with an injected clock.
   private:
    friend class Runtime;
    explicit Handle(std::weak_ptr<Inbox> inbox) : inbox_(std::move(inbox)) {}
    std::weak_ptr<Inbox> inbox_;
  };
  Runtime(Config config, State recovered, Persist persist, ApplyCommitted apply,
    Dispatch dispatch, RuntimeOptions options = {}, RuntimeClock clock = {});
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Handle handle() const;
  RuntimeStatus status() const;
  std::exception_ptr failure() const; // Original terminal error, if any.
  // request_stop is nonblocking; stop waits for active callbacks and joins.
  // Callbacks may request_stop, but must not stop/destroy their own runtime.
  void request_stop();
  void stop();
 private:
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx::raft
