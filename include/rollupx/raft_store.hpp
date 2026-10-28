#pragma once
#include "rollupx/raft_snapshot.hpp"
#include <memory>

namespace rollupx::raft {
enum class StoreOpenMode { create_or_open, existing_only };
enum class StoreCrashPoint { before_write, after_write, before_apply, after_apply,
  before_checkpoint, after_checkpoint, before_stage, after_stage, before_publish, after_publish };
struct SnapshotProgress { Hash id; std::uint64_t total_bytes, next_offset; bool complete; };
using StoreFailureHook = std::function<void(StoreCrashPoint)>;
// Real RocksDB implementation of Core's synchronous Persist callback. Owns an
// exclusive DB directory. Not the standalone transfer ledger or node identity DB.
class Store {
 public:
  Store(std::string directory, Manifest manifest, StoreFailureHook hook = {},
    StoreOpenMode mode = StoreOpenMode::create_or_open);
  // Ledger mode creates genesis and applied state atomically with the Raft image.
  // Existing core-only directories cannot be silently upgraded to ledger mode.
  Store(std::string directory, Manifest manifest, Genesis genesis, StoreFailureHook hook = {},
    StoreOpenMode mode = StoreOpenMode::create_or_open);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  State load() const;
  void persist(const State& next);
  Index apply_committed();
  LedgerState ledger_state() const;
  std::optional<LedgerReceipt> receipt(const Hash& id) const;
  std::optional<LedgerBatch> batch(Index index) const;
  // Storage-level recovery primitives. Invoke before attaching a Runtime: direct
  // installation behind a running Core would invalidate its recovered image.
  // No live InstallSnapshot handler or compaction is enabled by these methods.
  std::string create_snapshot(); // Audit, then synchronously publish local checkpoint.
  std::optional<std::string> local_snapshot() const;
  SnapshotProgress stage_snapshot(const SnapshotChunk&); // No active ledger effects.
  std::optional<SnapshotProgress> staged_snapshot() const;
  void discard_staged_snapshot();
  // Preserves local term/vote. Caller must first persist any greater leader term
  // through the normal protocol path. Rejects stale/conflicting committed history.
  bool publish_staged_snapshot(); // False for an identical already-applied prefix.
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx::raft
