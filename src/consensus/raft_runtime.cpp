#include "rollupx/raft_runtime.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace rollupx::raft {
namespace {
struct Tick {};
using Input = std::variant<Tick,Envelope,std::string>;
thread_local const void* active_owner = nullptr;
bool response(const Envelope& message) {
  return std::holds_alternative<VoteResponse>(message.message) || std::holds_alternative<AppendResponse>(message.message);
}
RuntimeClock clock_or_default(RuntimeClock clock) {
  if (clock) return clock;
  const auto start = std::chrono::steady_clock::now();
  return [start] { return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now()-start).count()); };
}
} // namespace
struct Runtime::Inbox {
  struct Event {
    Input input;
    std::promise<EventResult> promise;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::size_t bytes;
  };
  explicit Inbox(RuntimeOptions value) : options(value) {
    if (options.max_events == 0 || options.max_events > 4096 || options.max_bytes < sizeof(Event) ||
        options.max_bytes > 64*1024*1024 || options.tick_interval.count() < 1 || options.tick_interval.count() > 1000)
      throw std::invalid_argument("invalid Raft runtime bounds");
  }
  Ticket submit(Input input) {
    std::lock_guard lock(mutex);
    if (status.stopped || status.failed) return {};
    std::size_t bytes = sizeof(Event);
    const auto add = [&](std::size_t size) { bytes = size > options.max_bytes-bytes ? options.max_bytes+1 : bytes+size; };
    bool valid = true;
    if (const auto* command = std::get_if<std::string>(&input)) {
      valid = !command->empty() && command->size() <= kMaxCommandBytes;
      add(command->capacity());
    } else if (const auto* envelope = std::get_if<Envelope>(&input)) {
      if (const auto* append = std::get_if<AppendRequest>(&envelope->message)) {
        if (append->entries.size() > kMaxAppendEntries) {
          ++status.admission_rejected; return {Admission::invalid,{},{}};
        }
        if (append->entries.capacity() > options.max_bytes/sizeof(Entry)) bytes = options.max_bytes+1;
        else add(append->entries.capacity()*sizeof(Entry));
        std::size_t payload = 0;
        for (const auto& entry : append->entries) {
          if (bytes <= options.max_bytes) add(entry.command.capacity());
          if (entry.command.size() > kMaxCommandBytes || entry.command.size() > kMaxAppendBytes-payload) { valid = false; break; }
          payload += entry.command.size();
        }
      }
    }
    if (!valid || bytes > options.max_bytes) { ++status.admission_rejected; return {Admission::invalid,{},{}}; }
    if (status.pending_events == options.max_events || bytes > options.max_bytes-status.pending_bytes) {
      ++status.admission_rejected; return {Admission::full,{},{}};
    }
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    Event event{std::move(input),{},cancelled,bytes}; auto future = event.promise.get_future();
    queue.push_back(std::move(event)); ++status.pending_events; status.pending_bytes += bytes;
    wake.notify_one(); return {Admission::queued,std::move(future),std::move(cancelled)};
  }
  RuntimeOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<Event> queue;
  RuntimeStatus status;
  std::exception_ptr failure;
};
struct Runtime::Impl {
  Impl(Config config, State recovered, Persist persist, ApplyCommitted application, Dispatch output,
      RuntimeOptions options, RuntimeClock time)
    : inbox(std::make_shared<Inbox>(options)), clock(clock_or_default(std::move(time))),
      last_time(clock()), core(std::move(config),std::move(recovered),std::move(persist),last_time),
      apply(std::move(application)), dispatch(std::move(output)) {
    if (!apply || !dispatch) throw std::invalid_argument("runtime requires application and dispatch callbacks");
  }
  ~Impl() { stop(); }
  Millis now() {
    const auto value = clock();
    if (value < last_time) throw std::runtime_error("runtime clock moved backwards");
    last_time = value; return value;
  }
  void publish() {
    std::lock_guard lock(inbox->mutex); auto& status = inbox->status;
    status.role = core.role(); status.term = core.state().term; status.leader = core.leader();
    status.commit = core.state().commit; status.applied = applied; status.elections = core.elections();
  }
  void apply_progress() {
    const auto commit = core.state().commit;
    if (!application_started || applied != commit) {
      const auto progress = apply(commit);
      if (progress != commit) throw std::runtime_error("application did not durably reach commit index");
      applied = progress; application_started = true;
    }
  }
  EventResult process(const Input& input) {
    const auto time = now(); std::vector<Envelope> effects;
    EventResult result{Completion::processed,{},{},{}};
    try {
      if (std::holds_alternative<Tick>(input)) effects = core.tick(time);
      else if (const auto* message = std::get_if<Envelope>(&input)) effects = core.receive(*message,time);
      else { effects = core.propose(std::get<std::string>(input),time); result.proposed_index = core.state().log.size(); }
    } catch (const ProposalRejected&) {
      // Only the core's explicit admission error is nonfatal. A callback throwing
      // this same type still fails the core, and must not escape as rejection.
      if (core.failed()) throw;
      result.completion = Completion::rejected;
    }
    apply_progress();
    for (const auto& effect : effects) {
      if (response(effect)) {
        const auto* request = std::get_if<Envelope>(&input);
        if (!request || response(*request) || result.reply || effect.to != request->from || effect.from != request->to)
          throw std::runtime_error("unexpected unary Raft response");
        result.reply = effect;
      } else if (!dispatch(effect)) {
        std::lock_guard lock(inbox->mutex); ++inbox->status.outbound_dropped;
      }
    }
    publish(); return result;
  }
  void finish(Inbox::Event& event, EventResult result) {
    std::lock_guard lock(inbox->mutex);
    --inbox->status.pending_events; inbox->status.pending_bytes -= event.bytes;
    result.status = inbox->status; event.promise.set_value(std::move(result));
  }
  void fail() {
    publish(); std::lock_guard lock(inbox->mutex);
    inbox->failure = std::current_exception();
    inbox->status.failed = true; inbox->status.stopped = true;
    inbox->status.role = Role::follower; inbox->status.leader.reset();
  }
  void start() {
    std::promise<void> ready; auto future = ready.get_future();
    thread = std::thread([this, ready = std::move(ready)]() mutable {
      active_owner = this; // Thread-local guard, unaffected by OS thread-ID reuse.
      try { apply_progress(); publish(); ready.set_value(); }
      catch (...) { fail(); ready.set_exception(std::current_exception()); return; }
      auto next_tick = std::chrono::steady_clock::now()+inbox->options.tick_interval;
      for (;;) {
        std::optional<Inbox::Event> event;
        bool periodic_tick = false;
        {
          std::unique_lock lock(inbox->mutex);
          const auto available = [&] { return inbox->status.stopped || !inbox->queue.empty(); };
          if (inbox->options.automatic_ticks) inbox->wake.wait_until(lock,next_tick,available);
          else inbox->wake.wait(lock,available);
          if (inbox->status.stopped) break;
          if (inbox->options.automatic_ticks && std::chrono::steady_clock::now() >= next_tick) {
            next_tick = std::chrono::steady_clock::now()+inbox->options.tick_interval;
            periodic_tick = true;
          }
          if (!inbox->queue.empty()) {
            event = std::move(inbox->queue.front()); inbox->queue.pop_front();
          }
        }
        try {
          if (periodic_tick) (void)process(Input{Tick{}});
          if (event) {
            if (event->cancelled->load()) finish(*event,{Completion::cancelled,{},{},{}});
            else finish(*event,process(event->input));
          }
        } catch (...) {
          fail(); if (event) finish(*event,{Completion::failed,{},{},{}}); break;
        }
      }
      // Stop cancels queued work; an active operation has already completed.
      for (;;) {
        std::optional<Inbox::Event> event;
        {
          std::lock_guard lock(inbox->mutex);
          if (inbox->queue.empty()) break;
          event = std::move(inbox->queue.front()); inbox->queue.pop_front();
        }
        finish(*event,{Completion::stopped,{},{},{}});
      }
    });
    try { future.get(); } catch (...) { stop(); throw; }
  }
  void request_stop() {
    std::lock_guard lock(inbox->mutex); inbox->status.stopped = true; inbox->wake.notify_all();
  }
  void stop() {
    request_stop();
    if (active_owner == this) throw std::logic_error("owner callback cannot join its runtime");
    std::lock_guard lock(join_mutex); if (thread.joinable()) thread.join();
  }
  std::shared_ptr<Inbox> inbox;
  RuntimeClock clock;
  Millis last_time;
  Core core;
  ApplyCommitted apply;
  Dispatch dispatch;
  Index applied = 0;
  bool application_started = false;
  std::thread thread;
  std::mutex join_mutex;
};
Runtime::Runtime(Config config, State recovered, Persist persist, ApplyCommitted apply, Dispatch dispatch,
    RuntimeOptions options, RuntimeClock clock)
  : impl_(std::make_unique<Impl>(std::move(config),std::move(recovered),std::move(persist),std::move(apply),
      std::move(dispatch),options,std::move(clock))) { impl_->start(); }
Runtime::~Runtime() = default;
Runtime::Handle Runtime::handle() const { return Handle{impl_->inbox}; }
RuntimeStatus Runtime::status() const { std::lock_guard lock(impl_->inbox->mutex); return impl_->inbox->status; }
std::exception_ptr Runtime::failure() const { std::lock_guard lock(impl_->inbox->mutex); return impl_->inbox->failure; }
void Runtime::request_stop() { impl_->request_stop(); }
void Runtime::stop() { impl_->stop(); }
Ticket Runtime::Handle::receive(Envelope message) const {
  const auto inbox = inbox_.lock(); return inbox ? inbox->submit(std::move(message)) : Ticket{};
}
Ticket Runtime::Handle::propose(std::string command) const {
  const auto inbox = inbox_.lock(); return inbox ? inbox->submit(std::move(command)) : Ticket{};
}
Ticket Runtime::Handle::tick() const {
  const auto inbox = inbox_.lock(); return inbox ? inbox->submit(Tick{}) : Ticket{};
}
} // namespace rollupx::raft
