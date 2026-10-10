#pragma once
#include "rollupx/raft_store.hpp"
#include "ledger_fixture.hpp"

namespace rollupx::test {
inline raft::Manifest raft_manifest() {
  Hash genesis{}; genesis.back() = 1;
  return {"test-cluster",1,{1,2,3,4,5},genesis};
}
inline raft::Config raft_config() { return {1,{1,2,3,4,5},50,300,600,17}; }
inline raft::Manifest raft_ledger_manifest() {
  auto identity = raft_manifest(); const auto record = encode_genesis(ledger_genesis());
  identity.genesis_digest = keccak256({reinterpret_cast<const Byte*>(record.data()),record.size()}); return identity;
}
inline raft::State raft_ledger_history() {
  const auto transaction = ledger_transaction();
  return {1,{},{{1,{}},{1,encode_transfer_command({transaction,transaction})},{1,{}},
    {1,encode_transfer_command({transaction,ledger_transaction(1,1000),ledger_transaction(1,20)})}},4};
}
} // namespace rollupx::test
