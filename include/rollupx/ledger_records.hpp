#pragma once
#include "rollupx/execution.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rollupx {
inline constexpr std::size_t kMaxLedgerBatch = 1000;
struct Genesis {
  std::uint64_t chain_id;
  Accounts accounts;
  bool operator==(const Genesis&) const = default;
};
Genesis parse_genesis_text(std::string_view text);
struct LedgerState {
  std::uint64_t applied_index;
  Accounts accounts;
  bool operator==(const LedgerState&) const = default;
};
struct LedgerReceipt {
  Hash transaction_id;
  std::uint64_t log_index;
  std::uint32_t transaction_offset;
  TransferResult result;
  bool operator==(const LedgerReceipt&) const = default;
};
struct LedgerBatch {
  std::uint64_t index;
  std::vector<SignedTransaction> transactions;
  bool operator==(const LedgerBatch&) const = default;
};
// Versioned binary encodings with fixed-endian integers and Keccak integrity checks.
// Integrity digests are storage checksums, not Merkle state commitments.
std::string encode_genesis(const Genesis& value);
Genesis decode_genesis(std::string_view bytes);
std::string encode_state(const LedgerState& value);
LedgerState decode_state(std::string_view bytes);
std::string encode_receipt(const LedgerReceipt& value);
LedgerReceipt decode_receipt(std::string_view bytes);
std::string encode_batch(const LedgerBatch& value);
LedgerBatch decode_batch(std::string_view bytes);
std::string encode_progress(std::uint64_t index);
std::uint64_t decode_progress(std::string_view bytes);
std::string log_key(std::uint64_t index);
std::uint64_t decode_log_key(std::string_view key);
std::string receipt_key(const Hash& id);
// Fail-closed startup invariants, shared by the adapter and offline tests.
void validate_recovery_state(const Genesis& genesis, const LedgerState& state, std::uint64_t commit_index);
struct ApplicationPlan {
  LedgerState state;
  std::vector<LedgerReceipt> results;      // One per input, including retries.
  std::vector<LedgerReceipt> new_receipts; // Only first occurrences to write atomically.
};
using ReceiptLookup = std::function<std::optional<LedgerReceipt>(const Hash&)>;
using BatchLookup = std::function<std::optional<LedgerBatch>(std::uint64_t)>;
ApplicationPlan plan_application(const Genesis& genesis, const LedgerState& state,
  std::uint64_t commit_index, const LedgerBatch& batch, const ReceiptLookup& lookup);
// Streaming startup audit. Feed every retained log entry in key order, then finish
// before replaying any unapplied suffix. Holds constant state plus one batch plan.
// Any failed observation invalidates this instance; callers must abandon recovery.
class RecoveryAudit {
 public:
  RecoveryAudit(Genesis genesis, LedgerState stored, std::uint64_t commit_index,
    ReceiptLookup receipts, BatchLookup batches);
  void observe(std::uint64_t key_index, const LedgerBatch& batch);
  void finish();
 private:
  Genesis genesis_;
  LedgerState stored_;
  LedgerState replayed_;
  std::uint64_t commit_index_;
  std::uint64_t seen_ = 0;
  ReceiptLookup receipts_;
  BatchLookup batches_;
  bool closed_ = false;
};
} // namespace rollupx
