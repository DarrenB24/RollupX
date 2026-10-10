#include "rollupx/ledger_records.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>

namespace rollupx {
namespace {
constexpr std::size_t kMaxRecordBytes = 4 + 8 + 4 + kMaxLedgerBatch*kTransactionBytes + 32;
std::span<const Byte> bytes(std::string_view value) {
  return {reinterpret_cast<const Byte*>(value.data()), value.size()};
}
void append(std::string& out, std::span<const Byte> value) {
  out.append(reinterpret_cast<const char*>(value.data()), value.size());
}
void integer(std::string& out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) out.push_back(static_cast<char>(value >> (8*(width-1-i))));
}
std::string seal(std::string value) { const auto digest = keccak256(bytes(value)); append(value, digest); return value; }
class Reader {
 public:
  Reader(std::string_view value, std::string_view magic) {
    if (value.size() < magic.size()+32 || value.size() > kMaxRecordBytes || !value.starts_with(magic))
      throw std::invalid_argument("invalid ledger record size/version");
    const auto payload = value.substr(0, value.size()-32);
    const auto digest = keccak256(bytes(payload));
    if (!std::equal(digest.begin(), digest.end(), bytes(value).end()-32))
      throw std::invalid_argument("ledger record integrity failure");
    remaining_ = bytes(payload).subspan(magic.size());
  }
  std::span<const Byte> take(std::size_t count) {
    if (count > remaining_.size()) throw std::invalid_argument("truncated ledger record");
    const auto result = remaining_.first(count); remaining_ = remaining_.subspan(count); return result;
  }
  std::uint64_t integer(std::size_t width) {
    std::uint64_t result = 0;
    for (const auto byte : take(width)) result = (result << 8) | byte;
    return result;
  }
  void end() const { if (!remaining_.empty()) throw std::invalid_argument("trailing ledger record bytes"); }
 private:
  std::span<const Byte> remaining_;
};
void write_accounts(std::string& out, const Accounts& accounts) {
  for (const auto& account : accounts) { append(out, account.address); integer(out, account.balance, 8); integer(out, account.nonce, 8); }
}
Accounts read_accounts(Reader& reader) {
  Accounts result{};
  for (auto& account : result) {
    const auto address = reader.take(20); std::copy(address.begin(), address.end(), account.address.begin());
    account.balance = reader.integer(8); account.nonce = reader.integer(8);
  }
  return result;
}
Byte result_code(TransferResult result) {
  switch (result) {
    case TransferResult::applied: return 0;
    case TransferResult::wrong_chain: return 1;
    case TransferResult::zero_amount: return 2;
    case TransferResult::self_transfer: return 3;
    case TransferResult::unknown_account: return 4;
    case TransferResult::wrong_nonce: return 5;
    case TransferResult::insufficient_balance: return 6;
    case TransferResult::nonce_overflow: return 7;
    case TransferResult::balance_overflow: return 8;
  }
  throw std::invalid_argument("unknown receipt result");
}
TransferResult read_result(std::uint64_t code) {
  constexpr TransferResult results[] = {TransferResult::applied, TransferResult::wrong_chain,
    TransferResult::zero_amount, TransferResult::self_transfer, TransferResult::unknown_account,
    TransferResult::wrong_nonce, TransferResult::insufficient_balance, TransferResult::nonce_overflow,
    TransferResult::balance_overflow};
  if (code >= std::size(results)) throw std::invalid_argument("unknown stored receipt result");
  return results[code];
}
void check_receipt(const LedgerReceipt& value) {
  if (value.log_index == 0 || value.transaction_offset >= kMaxLedgerBatch)
    throw std::invalid_argument("invalid receipt position");
  (void)result_code(value.result);
}
} // namespace
Genesis parse_genesis_text(std::string_view text) {
  if (text.size() > 4096) throw std::invalid_argument("genesis file exceeds 4096 bytes");
  std::istringstream input{std::string(text)};
  const auto number = [](const std::string& token) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data()+token.size(), result);
    if (token.empty() || error != std::errc{} || end != token.data()+token.size())
      throw std::invalid_argument("invalid genesis uint64");
    return result;
  };
  std::string token, value;
  if (!(input >> token >> value) || token != "chain") throw std::invalid_argument("genesis begins with chain UINT64");
  Genesis result{number(value), {}}; std::size_t count = 0;
  while (input >> token) {
    std::string balance;
    if (token != "account" || !(input >> value >> balance) || count == result.accounts.size())
      throw std::invalid_argument("genesis requires at most 16 account ADDRESS BALANCE records");
    result.accounts[count++] = {fixed_hex<20>(value), number(balance), 0};
    if (result.accounts[count-1].address == Address{}) throw std::invalid_argument("explicit genesis address cannot be zero");
  }
  if (result.chain_id == 0) throw std::invalid_argument("zero chain ID");
  validate_accounts(result.accounts, true); return result;
}
std::string encode_genesis(const Genesis& value) {
  if (value.chain_id == 0) throw std::invalid_argument("zero chain ID");
  validate_accounts(value.accounts, true);
  std::string result = "RXG1"; integer(result, value.chain_id, 8); write_accounts(result, value.accounts); return seal(result);
}
Genesis decode_genesis(std::string_view value) {
  Reader reader(value, "RXG1"); Genesis result{reader.integer(8), read_accounts(reader)}; reader.end();
  if (result.chain_id == 0) throw std::invalid_argument("zero stored chain ID");
  validate_accounts(result.accounts, true); return result;
}
std::string encode_state(const LedgerState& value) {
  validate_accounts(value.accounts, false);
  std::string result = "RXS1"; integer(result, value.applied_index, 8); write_accounts(result, value.accounts); return seal(result);
}
LedgerState decode_state(std::string_view value) {
  Reader reader(value, "RXS1"); LedgerState result{reader.integer(8), read_accounts(reader)}; reader.end();
  validate_accounts(result.accounts, false); return result;
}
std::string encode_receipt(const LedgerReceipt& value) {
  check_receipt(value);
  std::string result = "RXR1"; append(result, value.transaction_id); integer(result, value.log_index, 8);
  integer(result, value.transaction_offset, 4); integer(result, result_code(value.result), 1); return seal(result);
}
LedgerReceipt decode_receipt(std::string_view value) {
  Reader reader(value, "RXR1"); LedgerReceipt result{};
  const auto id = reader.take(32); std::copy(id.begin(), id.end(), result.transaction_id.begin());
  result.log_index = reader.integer(8); result.transaction_offset = static_cast<std::uint32_t>(reader.integer(4));
  result.result = read_result(reader.integer(1)); reader.end(); check_receipt(result); return result;
}
std::string encode_batch(const LedgerBatch& value) {
  if (value.index == 0 || value.transactions.empty() || value.transactions.size() > kMaxLedgerBatch)
    throw std::invalid_argument("batch index/count out of bounds");
  std::string result = "RXB1"; integer(result, value.index, 8); integer(result, value.transactions.size(), 4);
  for (const auto& transaction : value.transactions) { (void)authenticate(transaction); append(result, encode_transaction(transaction)); }
  return seal(result);
}
LedgerBatch decode_batch(std::string_view value) {
  Reader reader(value, "RXB1"); LedgerBatch result{reader.integer(8), {}};
  const auto count = reader.integer(4);
  if (result.index == 0 || count == 0 || count > kMaxLedgerBatch) throw std::invalid_argument("stored batch index/count out of bounds");
  result.transactions.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) result.transactions.push_back(decode_transaction(reader.take(kTransactionBytes)));
  reader.end(); return result;
}
std::string encode_progress(std::uint64_t index) { std::string result = "RXP1"; integer(result, index, 8); return seal(result); }
std::uint64_t decode_progress(std::string_view value) { Reader reader(value, "RXP1"); const auto index = reader.integer(8); reader.end(); return index; }
std::string log_key(std::uint64_t index) {
  if (index == 0) throw std::invalid_argument("log indices begin at one");
  std::string result = "log/"; integer(result, index, 8); return result;
}
std::uint64_t decode_log_key(std::string_view key) {
  if (key.size() != 12 || !key.starts_with("log/")) throw std::invalid_argument("invalid log key");
  std::uint64_t index = 0;
  for (const auto byte : bytes(key.substr(4))) index = (index << 8) | byte;
  if (index == 0) throw std::invalid_argument("zero log index");
  return index;
}
std::string receipt_key(const Hash& id) { std::string result = "receipt/"; append(result, id); return result; }
void validate_recovery_state(const Genesis& genesis, const LedgerState& state, std::uint64_t commit_index) {
  if (genesis.chain_id == 0) throw std::invalid_argument("zero chain ID");
  validate_accounts(genesis.accounts, true); validate_accounts(state.accounts, false);
  if (state.applied_index > commit_index) throw std::invalid_argument("applied index exceeds commit index");
  std::uint64_t initial_supply = 0, current_supply = 0;
  for (std::size_t i = 0; i < genesis.accounts.size(); ++i) {
    if (state.accounts[i].address != genesis.accounts[i].address) throw std::invalid_argument("registry changed across restart");
    initial_supply += genesis.accounts[i].balance; current_supply += state.accounts[i].balance;
  }
  if (initial_supply != current_supply) throw std::invalid_argument("total supply changed across restart");
  if (state.applied_index == 0 && state.accounts != genesis.accounts) throw std::invalid_argument("unapplied state differs from genesis");
}
ApplicationPlan plan_application(const Genesis& genesis, const LedgerState& state,
    std::uint64_t commit_index, const LedgerBatch& batch, const ReceiptLookup& lookup) {
  validate_recovery_state(genesis, state, commit_index);
  if (!lookup || state.applied_index == std::numeric_limits<std::uint64_t>::max() ||
      batch.index != state.applied_index+1 || batch.index > commit_index ||
      batch.transactions.empty() || batch.transactions.size() > kMaxLedgerBatch)
    throw std::invalid_argument("cannot plan uncommitted, out-of-order or malformed batch");
  ApplicationPlan plan{{batch.index, state.accounts}, {}, {}};
  plan.results.reserve(batch.transactions.size()); plan.new_receipts.reserve(batch.transactions.size());
  std::map<Hash, LedgerReceipt> inserted;
  for (std::size_t offset = 0; offset < batch.transactions.size(); ++offset) {
    const auto& transaction = batch.transactions[offset]; const auto tx = authenticate(transaction);
    if (const auto prior = inserted.find(transaction.id); prior != inserted.end()) { plan.results.push_back(prior->second); continue; }
    if (const auto prior = lookup(transaction.id)) {
      check_receipt(*prior);
      if (prior->transaction_id != transaction.id || prior->log_index > state.applied_index)
        throw std::invalid_argument("receipt ID or progress mismatch during application");
      plan.results.push_back(*prior); continue;
    }
    const auto transition = execute_transfer(plan.state.accounts, genesis.chain_id, tx);
    const LedgerReceipt receipt{transaction.id, batch.index, static_cast<std::uint32_t>(offset), transition.result};
    inserted.emplace(transaction.id, receipt); plan.results.push_back(receipt); plan.new_receipts.push_back(receipt);
    plan.state.accounts = transition.accounts;
  }
  validate_recovery_state(genesis, plan.state, commit_index); return plan;
}
RecoveryAudit::RecoveryAudit(Genesis genesis, LedgerState stored, std::uint64_t commit_index,
    ReceiptLookup receipts, BatchLookup batches)
  : genesis_(std::move(genesis)), stored_(std::move(stored)), replayed_{0, genesis_.accounts},
    commit_index_(commit_index), receipts_(std::move(receipts)), batches_(std::move(batches)) {
  validate_recovery_state(genesis_, stored_, commit_index_);
  if (!receipts_ || !batches_) throw std::invalid_argument("recovery requires receipt and batch lookups");
}
void RecoveryAudit::observe(std::uint64_t key_index, const LedgerBatch& batch) {
  if (closed_) throw std::logic_error("recovery audit is closed");
  closed_ = true; // An error cannot be caught and skipped to accept partial history.
  if (seen_ == std::numeric_limits<std::uint64_t>::max() || key_index != seen_+1 ||
      key_index > commit_index_ || batch.index != key_index ||
      batch.transactions.empty() || batch.transactions.size() > kMaxLedgerBatch)
    throw std::invalid_argument("noncontiguous or malformed committed history");
  // Pending entries must be authenticated too, before recovery publishes any state.
  for (const auto& transaction : batch.transactions) (void)authenticate(transaction);
  if (key_index <= stored_.applied_index) {
    const auto plan = plan_application(genesis_, replayed_, commit_index_, batch, [&](const Hash& id) {
      const auto prior = receipts_(id);
      if (!prior || prior->log_index > replayed_.applied_index) return std::optional<LedgerReceipt>{};
      const auto original = batches_(prior->log_index);
      if (!original || original->index != prior->log_index ||
          prior->transaction_offset >= original->transactions.size() ||
          original->transactions[prior->transaction_offset].id != id)
        throw std::runtime_error("receipt does not identify its original command");
      return prior;
    });
    for (const auto& expected : plan.results) {
      const auto actual = receipts_(expected.transaction_id);
      if (!actual || *actual != expected) throw std::runtime_error("receipt differs from replayed history");
    }
    replayed_ = plan.state;
  }
  seen_ = key_index;
  closed_ = false;
}
void RecoveryAudit::finish() {
  if (closed_) throw std::logic_error("recovery audit is closed");
  closed_ = true;
  if (seen_ != commit_index_) throw std::runtime_error("committed log entry missing");
  if (replayed_ != stored_) throw std::runtime_error("stored state differs from committed history");
}
} // namespace rollupx
