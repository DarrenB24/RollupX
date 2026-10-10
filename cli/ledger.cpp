#include "rollupx/durable_ledger.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace rollupx;
void usage() {
  std::cerr << "rollupx-ledger --standalone DB_DIR GENESIS_FILE init|status\n"
    "rollupx-ledger --standalone DB_DIR GENESIS_FILE submit TRANSACTION_HEX\n"
    "rollupx-ledger --standalone DB_DIR GENESIS_FILE balance ADDRESS_HEX\n"
    "rollupx-ledger --standalone DB_DIR GENESIS_FILE receipt TRANSACTION_ID_HEX\n"
    "rollupx-ledger --standalone DB_DIR GENESIS_FILE batch INDEX\n"
    "Development only: direct local commitment, no Raft or Ethereum settlement.\n";
}
Genesis read_genesis(const char* path) {
  if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("genesis must be a regular file");
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open genesis");
  std::array<char, 4097> buffer{}; file.read(buffer.data(), buffer.size());
  if (file.bad()) throw std::runtime_error("cannot read genesis");
  return parse_genesis_text(std::string_view(buffer.data(), static_cast<std::size_t>(file.gcount())));
}
void print(const LedgerReceipt& receipt) {
  std::cout << "id=" << hex(receipt.transaction_id) << " result=" << result_name(receipt.result)
    << " log_index=" << receipt.log_index << " offset=" << receipt.transaction_offset << " durable=true\n";
}
} // namespace
int main(int argc, char* argv[]) {
  try {
    if (argc < 5 || std::string_view(argv[1]) != "--standalone") { usage(); return 2; }
    const std::string_view command(argv[4]);
    const bool no_argument = command == "init" || command == "status";
    const bool argument = command == "submit" || command == "balance" || command == "receipt" || command == "batch";
    if ((!no_argument && !argument) || argc != (no_argument ? 5 : 6)) { usage(); return 2; }
    const auto genesis = read_genesis(argv[3]);
    DurableLedger ledger(argv[2], genesis, {}, command == "init" ? LedgerOpenMode::create_or_open : LedgerOpenMode::existing_only);
    if (no_argument) {
      std::cout << "mode=standalone-development chain_id=" << genesis.chain_id
        << " applied_index=" << ledger.snapshot().applied_index << '\n'; return 0;
    }
    if (command == "submit") {
      const auto bytes = fixed_hex<kTransactionBytes>(argv[5]);
      print(ledger.submit(decode_transaction(bytes))); return 0;
    }
    if (command == "receipt") {
      const auto receipt = ledger.receipt(fixed_hex<32>(argv[5]));
      if (!receipt) { std::cerr << "receipt not found\n"; return 1; }
      print(*receipt); return 0;
    }
    if (command == "balance") {
      const auto wanted = fixed_hex<20>(argv[5]); const auto snapshot = ledger.snapshot();
      for (const auto& account : snapshot.accounts) if (account.address == wanted && wanted != Address{}) {
        std::cout << "address=" << hex(wanted) << " balance=" << account.balance << " nonce=" << account.nonce
          << " applied_index=" << snapshot.applied_index << '\n'; return 0;
      }
      std::cerr << "account not in genesis registry\n"; return 1;
    }
    std::uint64_t index = 0; const std::string_view input(argv[5]);
    const auto [end, error] = std::from_chars(input.data(), input.data()+input.size(), index);
    if (input.empty() || error != std::errc{} || end != input.data()+input.size()) throw std::invalid_argument("invalid batch index");
    const auto batch = ledger.batch(index);
    if (!batch) { std::cerr << "batch not found\n"; return 1; }
    std::cout << "index=" << batch->index << " transactions=" << batch->transactions.size() << '\n';
    for (const auto& tx : batch->transactions) std::cout << hex(encode_transaction(tx)) << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
