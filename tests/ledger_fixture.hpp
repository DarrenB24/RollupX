#pragma once
#include "rollupx/durable_ledger.hpp"

namespace rollupx::test {
inline PrivateKey ledger_key() {
  Hash bytes{}; bytes.back() = 1; return PrivateKey::from_bytes(bytes);
}
inline Genesis ledger_genesis() {
  Genesis result{31337, {}};
  result.accounts[0] = {address(ledger_key()), 100, 0};
  result.accounts[1] = {fixed_hex<20>("3535353535353535353535353535353535353535"), 0, 0};
  return result;
}
inline SignedTransaction ledger_transaction(std::uint64_t nonce = 0, std::uint64_t amount = 10) {
  const auto key = ledger_key(); const auto genesis = ledger_genesis();
  return sign_transfer(key, Transfer{genesis.chain_id, address(key), genesis.accounts[1].address, amount, nonce});
}
} // namespace rollupx::test
