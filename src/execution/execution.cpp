#include "rollupx/execution.hpp"
#include <limits>
#include <set>

namespace rollupx {
void validate_accounts(const Accounts& accounts, bool genesis) {
  std::set<Address> addresses;
  std::uint64_t total = 0;
  for (const auto& account : accounts) {
    if (account.address == Address{}) {
      if (account.balance != 0 || account.nonce != 0) throw std::invalid_argument("unused account slot contains state");
      continue;
    }
    if (!addresses.insert(account.address).second) throw std::invalid_argument("duplicate registry address");
    if (genesis && account.nonce != 0) throw std::invalid_argument("genesis nonce must be zero");
    if (account.balance > std::numeric_limits<std::uint64_t>::max()-total) throw std::invalid_argument("total supply overflow");
    total += account.balance;
  }
  if (addresses.empty()) throw std::invalid_argument("registry must contain an account");
}
Transition execute_transfer(const Accounts& accounts, std::uint64_t chain_id, const Transfer& tx) {
  const auto rejected = [&](TransferResult result) { return Transition{result, accounts}; };
  if (tx.chain_id != chain_id) return rejected(TransferResult::wrong_chain);
  if (tx.amount == 0) return rejected(TransferResult::zero_amount);
  if (tx.sender == tx.recipient) return rejected(TransferResult::self_transfer);
  std::size_t from = accounts.size(), to = accounts.size();
  for (std::size_t i = 0; i < accounts.size(); ++i) {
    if (accounts[i].address == Address{}) continue;
    if (accounts[i].address == tx.sender) from = i;
    if (accounts[i].address == tx.recipient) to = i;
  }
  if (from == accounts.size() || to == accounts.size()) return rejected(TransferResult::unknown_account);
  if (tx.nonce != accounts[from].nonce) return rejected(TransferResult::wrong_nonce);
  if (accounts[from].balance < tx.amount) return rejected(TransferResult::insufficient_balance);
  if (accounts[from].nonce == std::numeric_limits<std::uint64_t>::max()) return rejected(TransferResult::nonce_overflow);
  if (accounts[to].balance > std::numeric_limits<std::uint64_t>::max()-tx.amount) return rejected(TransferResult::balance_overflow);
  Transition result{TransferResult::applied, accounts};
  result.accounts[from].balance -= tx.amount; ++result.accounts[from].nonce;
  result.accounts[to].balance += tx.amount;
  return result;
}
ExecutionState::ExecutionState(std::uint64_t chain_id, Accounts genesis, std::size_t receipt_limit)
  : chain_id_(chain_id), accounts_(genesis), receipt_limit_(receipt_limit) {
  if (chain_id == 0 || receipt_limit == 0) throw std::invalid_argument("chain ID and receipt limit must be positive");
  validate_accounts(genesis, true);
}
TransferResult ExecutionState::apply(const SignedTransaction& transaction) {
  // Authenticate before trusting the ID, including retries. Invalid signatures are
  // admission errors, never deduplication hits or durable rejection receipts.
  const auto tx = authenticate(transaction);
  if (const auto found = receipts_.find(transaction.id); found != receipts_.end()) return found->second;
  if (receipts_.size() >= receipt_limit_) throw std::length_error("reference receipt capacity reached");
  const auto transition = execute_transfer(accounts_, chain_id_, tx);
  // Allocation can throw. Publish account changes only after receipt insertion succeeds.
  receipts_.emplace(transaction.id, transition.result);
  accounts_ = transition.accounts;
  return transition.result;
}
std::string_view result_name(TransferResult result) {
  switch (result) {
    case TransferResult::applied: return "applied";
    case TransferResult::wrong_chain: return "wrong_chain";
    case TransferResult::zero_amount: return "zero_amount";
    case TransferResult::self_transfer: return "self_transfer";
    case TransferResult::unknown_account: return "unknown_account";
    case TransferResult::wrong_nonce: return "wrong_nonce";
    case TransferResult::insufficient_balance: return "insufficient_balance";
    case TransferResult::nonce_overflow: return "nonce_overflow";
    case TransferResult::balance_overflow: return "balance_overflow";
  }
  throw std::logic_error("unknown transfer result");
}
} // namespace rollupx
