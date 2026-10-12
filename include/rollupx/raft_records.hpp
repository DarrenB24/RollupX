#pragma once
#include "rollupx/crypto.hpp"
#include "rollupx/raft.hpp"

namespace rollupx::raft {
struct Manifest {
  std::string cluster_id;
  NodeId node_id;
  std::vector<NodeId> members; // Canonical sorted identity mapping, never reordered on open.
  Hash genesis_digest;        // Keccak of the exact versioned genesis record.
  // Empty only for legacy RXM1 development stores. RXM2 binds sorted names to
  // IDs 1..N; wire-enabled construction requires this mapping.
  std::vector<std::string> member_names = {};
  bool operator==(const Manifest&) const = default;
};
inline constexpr std::size_t kMaxManifestBytes = 4+2+128+4+1+31*4+31*(1+128)+32+32;
inline constexpr std::size_t kMaxStateRecordBytes = 92 + 12*kMaxLogEntries + kMaxLogBytes;
Manifest make_named_manifest(std::string cluster, std::vector<std::string> members,
  Hash genesis, NodeId local);
// Shared peer configuration commitment. Requires named membership, excludes the
// local ID, and preserves the existing ROLLUPX_RAFT_WIRE_V1 preimage format.
Hash configuration_digest(const Manifest& manifest);
void validate_manifest(const Manifest& manifest);
std::string encode_manifest(const Manifest& manifest);
Manifest decode_manifest(std::string_view bytes);
std::string encode_state_record(const Manifest& manifest, const State& state);
State decode_state_record(const Manifest& manifest, std::string_view bytes);
// Reject regressions, re-voting, committed-prefix changes, same-term payload changes,
// and unsupported suffix-only deletion. Does not authorize application execution.
void validate_persisted_transition(const Manifest& manifest, const State& before, const State& after);
} // namespace rollupx::raft
