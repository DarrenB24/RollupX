#include "rollupx/raft.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace rollupx::raft {
namespace {
template<class T> void send(std::vector<Envelope>& out, NodeId from, NodeId to, T message) {
  out.push_back({from, to, std::move(message)});
}
void validate_append(const AppendRequest& request) {
  if (request.rpc == 0 || request.previous_index > kMaxLogEntries ||
      request.entries.size() > kMaxAppendEntries ||
      request.entries.size() > kMaxLogEntries-request.previous_index ||
      (request.previous_index == 0) != (request.previous_term == 0) || request.previous_term > request.term)
    throw std::invalid_argument("malformed AppendEntries bounds or previous position");
  std::size_t bytes = 0; Term previous = request.previous_term;
  for (const auto& entry : request.entries) {
    if (entry.term == 0 || entry.term < previous || entry.term > request.term || entry.command.size() > kMaxCommandBytes)
      throw std::invalid_argument("malformed AppendEntries entry");
    bytes += entry.command.size(); previous = entry.term;
  }
  if (bytes > kMaxAppendBytes) throw std::invalid_argument("AppendEntries payload too large");
}
} // namespace
Core::Core(Config config, State recovered, Persist persist, Millis now)
  : config_(std::move(config)), state_(std::move(recovered)), persist_(std::move(persist)),
    random_(config_.seed), now_(now), election_started_(now) {
  auto& members = config_.members;
  std::sort(members.begin(), members.end());
  validate_membership(config_.id, members);
  if (!persist_ || config_.heartbeat == 0 ||
      config_.election_min <= config_.heartbeat || config_.election_max < config_.election_min)
    throw std::invalid_argument("invalid Raft configuration");
  for (const auto member : members) if (member != config_.id) peers_.emplace(member, Progress{});
  validate_state(state_, members); reset_election();
}
void validate_membership(NodeId id, std::span<const NodeId> members) {
  if (id == 0 || members.size() < 3 || members.size() > 31 || members.size()%2 == 0 ||
      members.front() == 0 || !std::is_sorted(members.begin(), members.end()) ||
      std::adjacent_find(members.begin(), members.end()) != members.end() ||
      !std::binary_search(members.begin(), members.end(), id))
    throw std::invalid_argument("invalid Raft membership");
}
void validate_state(const State& state, std::span<const NodeId> members) {
  if (state.commit > state.log.size() || state.log.size() > kMaxLogEntries ||
      (state.voted_for && (state.term == 0 || !std::binary_search(members.begin(), members.end(), *state.voted_for))))
    throw std::invalid_argument("invalid recovered Raft progress or vote");
  std::size_t bytes = 0; Term previous = 0;
  for (const auto& entry : state.log) {
    if (entry.term == 0 || entry.term < previous || entry.term > state.term || entry.command.size() > kMaxCommandBytes)
      throw std::invalid_argument("invalid recovered Raft entry");
    bytes += entry.command.size(); previous = entry.term;
  }
  if (bytes > kMaxLogBytes) throw std::length_error("Raft log capacity reached; compaction required");
}
void Core::check_time(Millis now) {
  if (failed_) throw std::runtime_error("Raft core failed; recover from durable state");
  if (now < now_) throw std::invalid_argument("Raft clock moved backwards");
  now_ = now;
}
void Core::save(State state) {
  validate_state(state, config_.members);
  if (state == state_) return;
  try { persist_(state); state_ = std::move(state); }
  catch (...) { failed_ = true; role_ = Role::follower; leader_.reset(); throw; }
}
void Core::reset_election() {
  election_started_ = now_;
  // Explicit modulo mapping keeps seeded simulations identical across STL implementations.
  election_timeout_ = config_.election_min + random_()%(config_.election_max-config_.election_min+1);
}
void Core::follow(std::optional<NodeId> leader) {
  role_ = Role::follower; leader_ = leader; votes_.clear();
  for (auto& [id, progress] : peers_) { (void)id; progress = {}; }
}
void Core::higher_term(Term term) {
  if (term <= state_.term) return;
  auto next = state_; next.term = term; next.voted_for.reset(); save(std::move(next));
  follow(); // Rejected vote requests must not extend the election deadline.
}
Term Core::term_at(Index index) const { return index == 0 ? 0 : state_.log.at(index-1).term; }
bool Core::quorum_recent() const {
  std::size_t count = 1;
  for (const auto& [id, peer] : peers_) {
    (void)id;
    if (peer.contact && now_-*peer.contact < config_.election_max) ++count;
  }
  return count > config_.members.size()/2;
}
void Core::replicate(NodeId id, std::vector<Envelope>& out) {
  auto& peer = peers_.at(id);
  if (peer.flight) {
    if (now_-peer.retransmitted >= config_.heartbeat) {
      send(out, config_.id, id, *peer.flight); peer.retransmitted = now_;
    }
    return;
  }
  if (rpc_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Raft RPC sequence exhausted");
  AppendRequest request{state_.term, ++rpc_, peer.next-1, term_at(peer.next-1), {}, state_.commit};
  std::size_t bytes = 0;
  for (Index index = peer.next; index <= state_.log.size() && request.entries.size() < kMaxAppendEntries; ++index) {
    const auto& entry = state_.log[index-1];
    if (bytes+entry.command.size() > kMaxAppendBytes) break;
    request.entries.push_back(entry); bytes += entry.command.size();
  }
  peer.flight = request; peer.sent = now_; peer.retransmitted = now_;
  send(out, config_.id, id, std::move(request));
}
void Core::broadcast(std::vector<Envelope>& out) {
  for (const auto& [id, peer] : peers_) { (void)peer; replicate(id, out); }
}
bool Core::advance_commit() {
  std::vector<Index> matched{state_.log.size()};
  for (const auto& [id, peer] : peers_) { (void)id; matched.push_back(peer.matched); }
  std::sort(matched.begin(), matched.end()); const auto index = matched[matched.size()/2];
  if (index <= state_.commit || term_at(index) != state_.term) return false;
  auto next = state_; next.commit = index; save(std::move(next)); return true;
}
void Core::become_leader(std::vector<Envelope>& out) {
  auto next = state_; next.log.push_back({state_.term, {}}); save(std::move(next));
  role_ = Role::leader; leader_ = config_.id; leader_since_ = now_; heartbeat_at_ = now_;
  for (auto& [id, peer] : peers_) {
    peer = {}; peer.next = state_.log.size(); // Replicate the newly appended no-op first.
    if (votes_.contains(id)) peer.contact = election_started_;
  }
  broadcast(out);
}
std::vector<Envelope> Core::tick(Millis now) try {
  check_time(now); std::vector<Envelope> out;
  if (role_ == Role::leader) {
    if (now_-leader_since_ >= config_.election_max && !quorum_recent()) { follow(); reset_election(); return out; }
    if (now_-heartbeat_at_ >= config_.heartbeat) { broadcast(out); heartbeat_at_ = now_; }
  } else if (now_-election_started_ >= election_timeout_) {
    if (state_.term == std::numeric_limits<Term>::max()) throw std::overflow_error("Raft term exhausted");
    auto next = state_; ++next.term; next.voted_for = config_.id; save(std::move(next));
    role_ = Role::candidate; leader_.reset(); votes_ = {config_.id}; ++elections_; reset_election();
    for (const auto& [id, peer] : peers_) {
      (void)peer; send(out, config_.id, id, VoteRequest{state_.term, state_.log.size(), term_at(state_.log.size())});
    }
  }
  return out;
} catch (const std::bad_alloc&) {
  failed_ = true; role_ = Role::follower; leader_.reset(); throw;
}
std::vector<Envelope> Core::propose(std::string command, Millis now) try {
  check_time(now);
  if (role_ != Role::leader || !quorum_recent() || state_.commit == 0 || term_at(state_.commit) != state_.term)
    throw std::runtime_error("leader with a committed current-term barrier and recent quorum required");
  if (command.empty() || command.size() > kMaxCommandBytes) throw std::invalid_argument("invalid Raft command size");
  auto next = state_; next.log.push_back({state_.term, std::move(command)}); save(std::move(next));
  std::vector<Envelope> out; broadcast(out); return out;
} catch (const std::bad_alloc&) {
  failed_ = true; role_ = Role::follower; leader_.reset(); throw;
}
std::vector<Envelope> Core::receive(const Envelope& envelope, Millis now) try {
  check_time(now);
  if (envelope.to != config_.id || !peers_.contains(envelope.from)) throw std::invalid_argument("invalid Raft message membership");
  const auto term = std::visit([](const auto& message) { return message.term; }, envelope.message);
  if (term == 0) throw std::invalid_argument("Raft RPC term must be positive");
  if (const auto* request = std::get_if<AppendRequest>(&envelope.message)) validate_append(*request);
  if (const auto* request = std::get_if<VoteRequest>(&envelope.message)) {
    if ((request->last_index == 0) != (request->last_term == 0) || request->last_term > request->term || request->last_index > kMaxLogEntries)
      throw std::invalid_argument("malformed RequestVote log position");
  }
  higher_term(term);
  std::vector<Envelope> out;
  std::visit([&](const auto& message) { handle(envelope.from, message, out); }, envelope.message);
  return out;
} catch (const std::bad_alloc&) {
  failed_ = true; role_ = Role::follower; leader_.reset(); throw;
}
void Core::handle(NodeId from, const VoteRequest& request, std::vector<Envelope>& out) {
  const bool current = request.term == state_.term;
  const bool fresh = std::pair{request.last_term, request.last_index} >= std::pair{term_at(state_.log.size()), Index(state_.log.size())};
  const bool grant = current && fresh && (!state_.voted_for || state_.voted_for == from);
  if (grant) { auto next = state_; next.voted_for = from; save(std::move(next)); reset_election(); }
  send(out, config_.id, from, VoteResponse{state_.term, grant});
}
void Core::handle(NodeId from, const VoteResponse& response, std::vector<Envelope>& out) {
  if (role_ != Role::candidate || response.term != state_.term || !response.granted) return;
  votes_.insert(from);
  if (votes_.size() > config_.members.size()/2) become_leader(out);
}
void Core::handle(NodeId from, const AppendRequest& request, std::vector<Envelope>& out) {
  if (request.term < state_.term) {
    send(out, config_.id, from, AppendResponse{state_.term, request.rpc, false, 0, state_.log.size()+1}); return;
  }
  follow(from); reset_election();
  if (request.previous_index > state_.log.size()) {
    send(out, config_.id, from, AppendResponse{state_.term, request.rpc, false, 0, state_.log.size()+1}); return;
  }
  if (term_at(request.previous_index) != request.previous_term) {
    Index conflict = request.previous_index; const auto term = term_at(conflict);
    while (conflict > 1 && term_at(conflict-1) == term) --conflict;
    send(out, config_.id, from, AppendResponse{state_.term, request.rpc, false, 0, conflict}); return;
  }
  auto next = state_; Index index = request.previous_index;
  for (const auto& entry : request.entries) {
    ++index;
    if (index <= next.log.size() && next.log[index-1] != entry) {
      if (index <= next.commit || next.log[index-1].term == entry.term) {
        failed_ = true; throw std::runtime_error("conflicting committed entry or same-term command; stop node");
      }
      next.log.resize(index-1);
    }
    if (index > next.log.size()) next.log.push_back(entry);
  }
  next.commit = std::max(next.commit, std::min(request.leader_commit, index));
  save(std::move(next));
  send(out, config_.id, from, AppendResponse{state_.term, request.rpc, true, index, 0});
}
void Core::handle(NodeId from, const AppendResponse& response, std::vector<Envelope>& out) {
  if (role_ != Role::leader || response.term != state_.term) return;
  auto& peer = peers_.at(from);
  if (!peer.flight || response.rpc != peer.flight->rpc) return;
  const auto previous = peer.flight->previous_index;
  const auto last = previous+peer.flight->entries.size();
  if ((response.success && response.match_index != last) ||
      (!response.success && (response.conflict_index == 0 || response.conflict_index > previous)))
    throw std::invalid_argument("AppendEntries response contradicts request");
  peer.contact = peer.sent; // Delayed/duplicate replies cannot manufacture fresh quorum contact.
  peer.flight.reset();
  if (response.success) {
    peer.matched = std::max(peer.matched, last); peer.next = peer.matched+1;
    const bool committed = advance_commit();
    if (committed) broadcast(out);
    else if (peer.next <= state_.log.size()) replicate(from, out);
  } else {
    if (response.conflict_index <= peer.matched) { failed_ = true; throw std::runtime_error("peer lost acknowledged log"); }
    peer.next = response.conflict_index; replicate(from, out);
  }
}
} // namespace rollupx::raft
