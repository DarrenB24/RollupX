#pragma once
#include "rollupx/crypto.hpp"
#include "rollupx/raft.hpp"

namespace rollupx::raft {
struct Manifest {
  std::string cluster_id;
  NodeId node_id;
  std::vector<NodeId> members; // Canonical sorted identity mapping, never reordered on open.
  Hash genesis_digest;        // Keccak of the exact versioned genesis record.
  bool operator==(const Manifest&) const = default;
};
inline constexpr std::size_t kMaxStateRecordBytes = 92 + 12*kMaxLogEntries + kMaxLogBytes;
void validate_manifest(const Manifest& manifest);
std::string encode_manifest(const Manifest& manifest);
Manifest decode_manifest(std::string_view bytes);
std::string encode_state_record(const Manifest& manifest, const State& state);
State decode_state_record(const Manifest& manifest, std::string_view bytes);
// Reject regressions, re-voting, committed-prefix changes, same-term payload changes,
// and unsupported suffix-only deletion. Does not authorize application execution.
void validate_persisted_transition(const Manifest& manifest, const State& before, const State& after);
} // namespace rollupx::raft
