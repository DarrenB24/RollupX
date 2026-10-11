#include "rollupx/raft_application.hpp"
#include "../ledger_fixture.hpp"
#include "../raft_store_fixture.hpp"
#include <algorithm>
#include <gtest/gtest.h>

namespace {
using namespace rollupx;
using namespace rollupx::test;
class Application : public ::testing::Test {
 protected:
  Genesis genesis = ledger_genesis();
  LedgerState applied{0,genesis.accounts};
  raft::State consensus{1,{}, {},0};
  std::map<Hash,LedgerReceipt> receipts;
  ReceiptLookup lookup = [this](const Hash& id) -> std::optional<LedgerReceipt> {
    const auto found = receipts.find(id); return found == receipts.end() ? std::nullopt : std::optional{found->second};
  };
  void append(std::vector<SignedTransaction> transactions = {}) {
    consensus.log.push_back({1,transactions.empty() ? "" : encode_transfer_command(transactions)}); ++consensus.commit;
  }
  void apply_next() {
    const auto plan = plan_committed_entry(genesis,applied,consensus,applied.applied_index+1,lookup);
    for (const auto& receipt : plan.new_receipts) receipts.emplace(receipt.transaction_id,receipt);
    applied = plan.state;
  }
};
TEST_F(Application, CommandHasNoIndexAndAuthenticatesEveryEnvelope) {
  const auto transaction = ledger_transaction(); const auto command = encode_transfer_command({transaction,transaction});
  EXPECT_EQ(decode_transfer_command(command),(std::vector<SignedTransaction>{transaction,transaction}));
  EXPECT_EQ(command.size(),40+2*kTransactionBytes);
  EXPECT_THROW(encode_transfer_command({}),std::invalid_argument);
  EXPECT_THROW(encode_transfer_command(std::vector<SignedTransaction>(1001,transaction)),std::invalid_argument);
  auto bad = transaction; bad.signature[64] = 3;
  EXPECT_THROW(encode_transfer_command({transaction,bad}),std::invalid_argument);
  for (std::size_t i = 0; i < command.size(); ++i) {
    auto changed = command; changed[i] ^= 1; EXPECT_THROW(decode_transfer_command(changed),std::invalid_argument);
  }
  EXPECT_THROW(decode_transfer_command(command+"x"),std::invalid_argument);
  EXPECT_THROW(decode_transfer_command(encode_batch({1,{transaction}})),std::invalid_argument);
}
TEST_F(Application, NoopsAdvanceProgressAndRetriesKeepOriginalPosition) {
  const auto first = ledger_transaction(); append(); append({first,first}); append();
  append({first,ledger_transaction(1,20)}); append();
  while (applied.applied_index < consensus.commit) apply_next();
  EXPECT_EQ(applied.applied_index,5U); EXPECT_EQ(applied.accounts[0].balance,70U);
  EXPECT_EQ(applied.accounts[0].nonce,2U); ASSERT_EQ(receipts.size(),2U);
  EXPECT_EQ(receipts.at(first.id).log_index,2U); EXPECT_EQ(receipts.at(first.id).transaction_offset,0U);
  EXPECT_NO_THROW(audit_raft_application(genesis,applied,consensus,lookup));
}
TEST_F(Application, ValidChecksumDoesNotAuthorizeMalformedCommandsOrSignatures) {
  const auto original = encode_transfer_command({ledger_transaction()});
  const auto checksum = [](std::string command) {
    const auto digest = keccak256({reinterpret_cast<const Byte*>(command.data()),command.size()-32});
    std::copy(digest.begin(),digest.end(),command.end()-32);
    return command;
  };
  for (const std::uint32_t count : {0U,2U,1001U,0xffffffffU}) {
    auto command = original;
    for (unsigned i = 0; i < 4; ++i) command[4+i] = static_cast<char>(count >> (24-8*i));
    EXPECT_THROW(decode_transfer_command(checksum(command)),std::invalid_argument);
  }
  // Mutate the envelope ID, signed payload, signature scalar, and recovery ID.
  for (const auto offset : {8U,40U,117U,181U}) {
    auto command = original; command[offset] ^= 4;
    EXPECT_THROW(decode_transfer_command(checksum(command)),std::invalid_argument);
  }
}
TEST_F(Application, AuditRejectsRepointingReceiptToALaterDuplicate) {
  const auto transaction = ledger_transaction();
  append({transaction,transaction}); append({transaction}); apply_next(); apply_next();
  receipts.at(transaction.id).transaction_offset = 1;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
  receipts.at(transaction.id).transaction_offset = 0;
  receipts.at(transaction.id).log_index = 2;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
}
TEST_F(Application, AuditRejectsForgedSuccessAndRejectionResults) {
  const auto valid = ledger_transaction(); const auto rejected = ledger_transaction(1,1000);
  append({valid,rejected}); apply_next();
  receipts.at(rejected.id).result = TransferResult::applied;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
  receipts.at(rejected.id).result = TransferResult::insufficient_balance;
  receipts.at(valid.id).result = TransferResult::insufficient_balance;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
}
TEST_F(Application, EveryAppliedBoundaryReplaysCommittedSuffixExactlyOnce) {
  const auto first = ledger_transaction(); append(); append({first}); append();
  append({first,ledger_transaction(1,1000),ledger_transaction(1,20)});
  for (raft::Index boundary = 0; boundary <= consensus.commit; ++boundary) {
    applied = {0,genesis.accounts}; receipts.clear();
    while (applied.applied_index < boundary) apply_next();
    EXPECT_NO_THROW(audit_raft_application(genesis,applied,consensus,lookup));
    while (applied.applied_index < consensus.commit) {
      apply_next(); EXPECT_NO_THROW(audit_raft_application(genesis,applied,consensus,lookup));
    }
    EXPECT_EQ(applied.accounts[0].balance,70U); EXPECT_EQ(applied.accounts[0].nonce,2U); EXPECT_EQ(receipts.size(),3U);
  }
}
TEST_F(Application, UncommittedAndOutOfOrderEntriesNeverApply) {
  append({ledger_transaction()}); consensus.commit = 0;
  EXPECT_THROW(apply_next(),std::invalid_argument); EXPECT_EQ(applied.accounts,genesis.accounts);
  consensus.commit = 1;
  EXPECT_THROW(plan_committed_entry(genesis,applied,consensus,2,lookup),std::invalid_argument);
  consensus.commit = 2; EXPECT_THROW(apply_next(),std::invalid_argument);
}
TEST_F(Application, ForgedFutureNoopAndWrongOffsetReceiptsFail) {
  const auto transaction = ledger_transaction(); append(); append({transaction}); apply_next();
  for (const auto& receipt : {LedgerReceipt{transaction.id,1,0,TransferResult::applied},
       LedgerReceipt{transaction.id,2,0,TransferResult::applied},LedgerReceipt{transaction.id,1,2,TransferResult::applied}}) {
    receipts[transaction.id] = receipt; EXPECT_THROW(apply_next(),std::invalid_argument);
  }
  EXPECT_EQ(applied.applied_index,1U); EXPECT_EQ(applied.accounts,genesis.accounts);
}
TEST_F(Application, AuditRejectsMissingReceiptsAndSupplyPreservingCorruption) {
  const auto transaction = ledger_transaction(); append(); append({transaction}); apply_next(); apply_next();
  const auto receipt = receipts.at(transaction.id); receipts.clear();
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
  receipts[transaction.id] = receipt; ++applied.accounts[0].nonce;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
  --applied.accounts[0].nonce; --applied.accounts[0].balance; ++applied.accounts[1].balance;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::runtime_error);
}
TEST_F(Application, AuditAuthenticatesEntirePendingCommittedSuffixBeforeReplay) {
  append(); append({ledger_transaction()}); consensus.log[1].command[10] ^= 1;
  EXPECT_THROW(audit_raft_application(genesis,applied,consensus,lookup),std::invalid_argument);
  EXPECT_EQ(applied.applied_index,0U); EXPECT_TRUE(receipts.empty());
}
TEST_F(Application, GenesisMustMatchManifestCommitment) {
  auto identity = raft_manifest(); EXPECT_THROW(validate_ledger_identity(identity,genesis),std::invalid_argument);
  const auto record = encode_genesis(genesis);
  identity.genesis_digest = keccak256({reinterpret_cast<const Byte*>(record.data()),record.size()});
  EXPECT_NO_THROW(validate_ledger_identity(identity,genesis));
  ++genesis.chain_id; EXPECT_THROW(validate_ledger_identity(identity,genesis),std::invalid_argument);
}
} // namespace
