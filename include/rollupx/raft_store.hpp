#pragma once
#include "rollupx/raft_application.hpp"
#include <memory>

namespace rollupx::raft {
enum class StoreOpenMode { create_or_open, existing_only };
enum class StoreCrashPoint { before_write, after_write, before_apply, after_apply };
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
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx::raft
