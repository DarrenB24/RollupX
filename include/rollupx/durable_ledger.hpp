#pragma once
#include "rollupx/ledger_records.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace rollupx {
enum class LedgerCrashPoint { before_commit, after_commit, before_apply, after_apply };
using LedgerFailureHook = std::function<void(LedgerCrashPoint)>;
enum class LedgerOpenMode { create_or_open, existing_only };

// Standalone DEVELOPMENT ledger. Not a Raft commit authority and never attached to
// the cluster API. Uses a distinct on-disk manifest so a node cannot open this DB.
class DurableLedger {
 public:
  DurableLedger(std::string directory, Genesis genesis, LedgerFailureHook hook = {},
    LedgerOpenMode mode = LedgerOpenMode::create_or_open);
  ~DurableLedger();
  DurableLedger(const DurableLedger&) = delete;
  DurableLedger& operator=(const DurableLedger&) = delete;
  // Returns only after synchronous account/receipt/progress application. Rejection
  // receipts are also durable. Invalid signatures throw before log commitment.
  LedgerReceipt submit(const SignedTransaction& transaction);
  std::vector<LedgerReceipt> commit_development_batch(const std::vector<SignedTransaction>& transactions);
  [[nodiscard]] LedgerState snapshot() const;
  [[nodiscard]] std::optional<LedgerReceipt> receipt(const Hash& id) const;
  [[nodiscard]] std::optional<LedgerBatch> batch(std::uint64_t index) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx
