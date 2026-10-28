#pragma once
#include "rollupx/raft_application.hpp"
#include <map>

namespace rollupx::raft {
// First snapshot format retains the signed committed history for independent
// receipt/state auditing. It does not authorize log compaction or increase limits.
inline constexpr std::size_t kMaxSnapshotBytes = 16*1024*1024;
inline constexpr std::size_t kMaxSnapshotChunkBytes = 256*1024;
struct LedgerSnapshot {
  State prefix; // Fully committed, term == last entry term, no vote; no suffix.
  LedgerState ledger;
  std::map<Hash,LedgerReceipt> receipts; // Original results, including rejections.
  bool operator==(const LedgerSnapshot&) const = default;
};
Hash snapshot_hash(std::string_view bytes);
std::string encode_snapshot(const Manifest&, const Genesis&, const LedgerSnapshot&);
LedgerSnapshot decode_snapshot(const Manifest&, const Genesis&, std::string_view);
LedgerSnapshot make_snapshot(const Manifest&, const Genesis&, const State&, const LedgerState&, const ReceiptLookup&);

struct SnapshotChunk {
  Hash id;
  std::uint64_t total_bytes, offset;
  std::string data;
};
struct SnapshotStage {
  Hash id;
  std::uint64_t total_bytes;
  std::string data;
  bool complete() const { return data.size() == total_bytes; }
};
std::string encode_snapshot_stage(const SnapshotStage&);
SnapshotStage decode_snapshot_stage(std::string_view);
// Pure admission: exact duplicates are idempotent; holes, overlap and changed
// bytes are rejected. Offset zero may start a new generation with a different ID.
SnapshotStage append_snapshot_chunk(const std::optional<SnapshotStage>&, const SnapshotChunk&);
} // namespace rollupx::raft
