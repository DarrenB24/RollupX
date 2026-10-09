#pragma once
#include "rollupx/transaction.hpp"
#include <array>
#include <map>

namespace rollupx {
struct Account {
  Address address{};
  std::uint64_t balance{}, nonce{};
  bool operator==(const Account&) const = default;
};
using Accounts = std::array<Account, 16>;
enum class TransferResult {
  applied, wrong_chain, zero_amount, self_transfer, unknown_account,
  wrong_nonce, insufficient_balance, nonce_overflow, balance_overflow
};
std::string_view result_name(TransferResult result);
struct Transition {
  TransferResult result;
  Accounts accounts;
};
// Pure, deterministic checked transition. State must already pass validate_accounts.
Transition execute_transfer(const Accounts& accounts, std::uint64_t chain_id, const Transfer& transfer);
void validate_accounts(const Accounts& accounts, bool genesis);

// In-memory reference state machine; NOT a durable ledger or a client confirmation.
// Single owner mutates it; signature verification functions are independently thread-safe.
// The bounded receipt set models deduplication until RocksDB integration is verified.
class ExecutionState {
 public:
  ExecutionState(std::uint64_t chain_id, Accounts genesis, std::size_t receipt_limit = 100000);
  TransferResult apply(const SignedTransaction& transaction);
  [[nodiscard]] const Accounts& accounts() const { return accounts_; }
  [[nodiscard]] std::size_t receipt_count() const { return receipts_.size(); }
 private:
  std::uint64_t chain_id_;
  Accounts accounts_;
  const std::size_t receipt_limit_;
  std::map<Hash, TransferResult> receipts_;
};
} // namespace rollupx
