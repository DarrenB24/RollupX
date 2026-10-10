#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rollupx::raft {
using NodeId = std::uint32_t;
using Term = std::uint64_t;
using Index = std::uint64_t;
using Millis = std::uint64_t;
enum class Role { follower, candidate, leader };
struct Entry {
  Term term = 0;
  std::string command; // Empty is a Raft no-op, never an application command.
  bool operator==(const Entry&) const = default;
};
struct State {
  Term term = 0;
  std::optional<NodeId> voted_for;
  std::vector<Entry> log; // Index 1 at log[0]; no snapshots/compaction yet.
  Index commit = 0;
  bool operator==(const State&) const = default;
};
struct Config {
  NodeId id;
  std::vector<NodeId> members;
  Millis heartbeat = 50, election_min = 300, election_max = 600;
  std::uint64_t seed = 1;
};
inline constexpr std::size_t kMaxLogEntries = 4096;
inline constexpr std::size_t kMaxLogBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxCommandBytes = 174048;
inline constexpr std::size_t kMaxAppendEntries = 16;
inline constexpr std::size_t kMaxAppendBytes = 512 * 1024;
// Shared by the protocol and durable record boundary; members must be sorted.
void validate_membership(NodeId id, std::span<const NodeId> members);
void validate_state(const State& state, std::span<const NodeId> members);
struct VoteRequest { Term term; Index last_index; Term last_term; };
struct VoteResponse { Term term; bool granted; };
struct AppendRequest {
  Term term;
  std::uint64_t rpc;
  Index previous_index;
  Term previous_term;
  std::vector<Entry> entries;
  Index leader_commit;
};
struct AppendResponse {
  Term term;
  std::uint64_t rpc;
  bool success;
  Index match_index;
  Index conflict_index;
};
using Message = std::variant<VoteRequest, VoteResponse, AppendRequest, AppendResponse>;
struct Envelope { NodeId from, to; Message message; };
// Must synchronously/atomically persist the entire supplied state before returning.
// Throw on any ambiguous failure. The core then fails closed until reconstructed.
// The simulation's memory recorder is NOT a durable implementation of this contract.
using Persist = std::function<void(const State&)>;
class Core {
 public:
  // Single event-loop owner. No internal threads, clock reads, networking, or disk IO.
  Core(Config config, State recovered, Persist persist, Millis now = 0);
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  std::vector<Envelope> tick(Millis now);
  std::vector<Envelope> receive(const Envelope& message, Millis now);
  std::vector<Envelope> propose(std::string command, Millis now);
  [[nodiscard]] const State& state() const { return state_; }
  [[nodiscard]] Role role() const { return role_; }
  [[nodiscard]] std::optional<NodeId> leader() const { return leader_; }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] std::uint64_t elections() const { return elections_; }
 private:
  struct Progress {
    Index next = 1, matched = 0;
    std::optional<Millis> contact;
    std::optional<AppendRequest> flight;
    Millis sent = 0, retransmitted = 0;
  };
  void check_time(Millis now);
  void save(State state);
  void reset_election();
  void follow(std::optional<NodeId> leader = {});
  void higher_term(Term term);
  void become_leader(std::vector<Envelope>& out);
  void replicate(NodeId peer, std::vector<Envelope>& out);
  void broadcast(std::vector<Envelope>& out);
  bool quorum_recent() const;
  bool advance_commit();
  Term term_at(Index index) const;
  void handle(NodeId from, const VoteRequest& message, std::vector<Envelope>& out);
  void handle(NodeId from, const VoteResponse& message, std::vector<Envelope>& out);
  void handle(NodeId from, const AppendRequest& message, std::vector<Envelope>& out);
  void handle(NodeId from, const AppendResponse& message, std::vector<Envelope>& out);
  Config config_;
  State state_;
  Persist persist_;
  std::mt19937_64 random_;
  Millis now_, election_started_, election_timeout_ = 0, leader_since_ = 0, heartbeat_at_ = 0;
  Role role_ = Role::follower;
  std::optional<NodeId> leader_;
  std::map<NodeId, Progress> peers_;
  std::set<NodeId> votes_;
  std::uint64_t rpc_ = 0, elections_ = 0;
  bool failed_ = false;
};
} // namespace rollupx::raft
