#include "rollupx/raft_application.hpp"
#include <algorithm>
#include <limits>

namespace rollupx {
namespace {
std::span<const Byte> bytes(std::string_view input) {
  return {reinterpret_cast<const Byte*>(input.data()),input.size()};
}
} // namespace
std::string encode_transfer_command(const std::vector<SignedTransaction>& transactions) {
  if (transactions.empty() || transactions.size() > kMaxLedgerBatch) throw std::invalid_argument("invalid transfer command count");
  std::string out = "RXC1";
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(transactions.size() >> shift));
  for (const auto& transaction : transactions) {
    (void)authenticate(transaction); const auto encoded = encode_transaction(transaction);
    out.append(reinterpret_cast<const char*>(encoded.data()),encoded.size());
  }
  const auto digest = keccak256(bytes(out)); out.append(reinterpret_cast<const char*>(digest.data()),digest.size());
  return out;
}
std::vector<SignedTransaction> decode_transfer_command(std::string_view command) {
  if (command.size() < 40+kTransactionBytes || command.size() > 40+kMaxLedgerBatch*kTransactionBytes || !command.starts_with("RXC1"))
    throw std::invalid_argument("invalid transfer command size/version");
  const auto digest = keccak256(bytes(command.substr(0,command.size()-32)));
  if (!std::equal(digest.begin(),digest.end(),bytes(command).end()-32)) throw std::invalid_argument("transfer command checksum mismatch");
  std::uint32_t count = 0;
  for (const auto value : bytes(command.substr(4,4))) count = (count << 8) | value;
  if (count == 0 || count > kMaxLedgerBatch || command.size() != 40+count*kTransactionBytes)
    throw std::invalid_argument("transfer command count/length mismatch");
  std::vector<SignedTransaction> result; result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) result.push_back(decode_transaction(bytes(command.substr(8+i*kTransactionBytes,kTransactionBytes))));
  return result;
}
void validate_ledger_identity(const raft::Manifest& manifest, const Genesis& genesis) {
  raft::validate_manifest(manifest);
  if (keccak256(bytes(encode_genesis(genesis))) != manifest.genesis_digest)
    throw std::invalid_argument("Raft identity does not bind this ledger genesis");
}
void validate_receipt_origin(const LedgerReceipt& receipt, const Hash& id,
    const LedgerState& applied, const raft::State& consensus) {
  (void)encode_receipt(receipt); // Validates result enum and fixed position bounds.
  if (receipt.transaction_id != id || receipt.log_index > applied.applied_index ||
      receipt.log_index > consensus.commit || receipt.log_index > consensus.log.size())
    throw std::invalid_argument("receipt identity or applied progress mismatch");
  const auto transactions = decode_transfer_command(consensus.log[receipt.log_index-1].command);
  if (receipt.transaction_offset >= transactions.size() || transactions[receipt.transaction_offset].id != id)
    throw std::invalid_argument("receipt does not identify its original transfer");
}
ApplicationPlan plan_committed_entry(const Genesis& genesis, const LedgerState& applied,
    const raft::State& consensus, raft::Index index, const ReceiptLookup& lookup) {
  validate_recovery_state(genesis,applied,consensus.commit);
  if (!lookup || consensus.commit > consensus.log.size() || applied.applied_index == std::numeric_limits<raft::Index>::max() ||
      index != applied.applied_index+1 || index > consensus.commit)
    throw std::invalid_argument("cannot apply an uncommitted or out-of-order Raft entry");
  const auto& entry = consensus.log[index-1];
  if (entry.command.empty()) return {{index,applied.accounts},{},{}};
  const auto transactions = decode_transfer_command(entry.command);
  return plan_application(genesis,applied,consensus.commit,{index,transactions},[&](const Hash& id) {
    const auto prior = lookup(id);
    if (prior) validate_receipt_origin(*prior,id,applied,consensus);
    return prior;
  });
}
void audit_raft_application(const Genesis& genesis, const LedgerState& applied,
    const raft::State& consensus, const ReceiptLookup& lookup) {
  validate_recovery_state(genesis,applied,consensus.commit);
  if (!lookup || consensus.commit > consensus.log.size()) throw std::invalid_argument("invalid application audit inputs");
  LedgerState replayed{0,genesis.accounts};
  for (raft::Index index = 1; index <= consensus.commit; ++index) {
    const auto& command = consensus.log[index-1].command;
    if (!command.empty()) {
      for (const auto& transaction : decode_transfer_command(command)) {
        const auto prior = lookup(transaction.id);
        if (prior) validate_receipt_origin(*prior,transaction.id,applied,consensus);
      }
    }
    if (index > applied.applied_index) continue;
    const auto plan = plan_committed_entry(genesis,replayed,consensus,index,[&](const Hash& id) {
      const auto prior = lookup(id);
      return prior && prior->log_index <= replayed.applied_index ? prior : std::optional<LedgerReceipt>{};
    });
    for (const auto& expected : plan.results) {
      const auto actual = lookup(expected.transaction_id);
      if (!actual || *actual != expected) throw std::runtime_error("Raft application receipt differs from replay");
    }
    replayed = plan.state;
  }
  if (replayed != applied) throw std::runtime_error("Raft application state differs from replay");
}
} // namespace rollupx
