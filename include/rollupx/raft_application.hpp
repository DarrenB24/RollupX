#pragma once
#include "rollupx/ledger_records.hpp"
#include "rollupx/raft_records.hpp"

namespace rollupx {
// RXC1 commands carry transactions, never a caller-selected Raft index. An empty
// Raft command is a no-op and is not accepted by these batch encode/decode functions.
std::string encode_transfer_command(const std::vector<SignedTransaction>& transactions);
std::vector<SignedTransaction> decode_transfer_command(std::string_view command);
void validate_ledger_identity(const raft::Manifest& manifest, const Genesis& genesis);
void validate_receipt_origin(const LedgerReceipt& receipt, const Hash& id,
  const LedgerState& applied, const raft::State& consensus);
ApplicationPlan plan_committed_entry(const Genesis& genesis, const LedgerState& applied,
  const raft::State& consensus, raft::Index index, const ReceiptLookup& lookup);
// Audit before replay. Checks all committed command bytes and recomputes the
// already-applied prefix, including no-ops, against receipts and account state.
void audit_raft_application(const Genesis& genesis, const LedgerState& applied,
  const raft::State& consensus, const ReceiptLookup& lookup);
} // namespace rollupx
