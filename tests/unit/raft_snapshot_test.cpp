#include "../snapshot_fixture.hpp"
#include <gtest/gtest.h>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
std::string reseal(std::string body) {
  const auto hash = snapshot_hash(body); body.append(reinterpret_cast<const char*>(hash.data()),hash.size()); return body;
}
TEST(SnapshotCodec, PortableBetweenMembersAndRetainsFirstRejectionAndAppliedReceipts) {
  const auto snapshot = ledger_snapshot(); const auto image = snapshot_image();
  auto other = raft_ledger_manifest(); other.node_id = 2;
  EXPECT_EQ(decode_snapshot(other,ledger_genesis(),image),snapshot);
  EXPECT_EQ(encode_snapshot(other,ledger_genesis(),snapshot),image);
  EXPECT_EQ(snapshot.receipts.size(),3U);
  EXPECT_EQ(snapshot.receipts.at(ledger_transaction().id).log_index,2U);
  EXPECT_EQ(snapshot.receipts.at(ledger_transaction(1,1000).id).result,TransferResult::insufficient_balance);
  EXPECT_EQ(snapshot.ledger.accounts[0].balance,70U);
  EXPECT_EQ(snapshot.ledger.accounts[0].nonce,2U);
}
TEST(SnapshotCodec, ExcludesUncommittedSuffixAndElectionMetadata) {
  auto state = ledger_snapshot().prefix; state.term = 5; state.voted_for = 3;
  state.log.push_back({5,encode_transfer_command({ledger_transaction(2,7)})});
  const auto original = ledger_snapshot();
  const auto snapshot = make_snapshot(raft_ledger_manifest(),ledger_genesis(),state,original.ledger,
    [&](const Hash& id) -> std::optional<LedgerReceipt> {
      const auto found = original.receipts.find(id);
      return found == original.receipts.end() ? std::nullopt : std::optional{found->second};
    });
  EXPECT_EQ(snapshot,original); EXPECT_EQ(snapshot.prefix.term,1U); EXPECT_FALSE(snapshot.prefix.voted_for);
  EXPECT_THROW(make_snapshot(raft_ledger_manifest(),ledger_genesis(),state,LedgerState{0,ledger_genesis().accounts},
    [](const Hash&) { return std::optional<LedgerReceipt>{}; }),std::invalid_argument);
  EXPECT_THROW(make_snapshot(raft_ledger_manifest(),ledger_genesis(),state,original.ledger,
    [](const Hash&) { return std::optional<LedgerReceipt>{}; }),std::runtime_error);
}
TEST(SnapshotCodec, RejectsCorruptionTruncationForeignIdentityAndForgedStateWithValidChecksums) {
  const auto image = snapshot_image(); const auto identity = raft_ledger_manifest(); const auto genesis = ledger_genesis();
  for (const auto length : {std::size_t{0},std::size_t{4},image.size()-1}) {
    EXPECT_THROW(decode_snapshot(identity,genesis,image.substr(0,length)),std::invalid_argument);
  }
  auto corrupt = image; corrupt[50] ^= 1;
  EXPECT_THROW(decode_snapshot(identity,genesis,corrupt),std::invalid_argument);
  auto other = identity; other.cluster_id = "foreign";
  EXPECT_THROW(decode_snapshot(other,genesis,image),std::invalid_argument);
  auto foreign_genesis = genesis; ++foreign_genesis.chain_id;
  EXPECT_THROW(decode_snapshot(identity,foreign_genesis,image),std::invalid_argument);
  const auto snapshot = ledger_snapshot();
  auto forged = snapshot.ledger; ++forged.accounts[0].balance; --forged.accounts[1].balance;
  const auto original_record = encode_state(snapshot.ledger);
  auto body = image.substr(0,image.size()-32); const auto position = body.find(original_record);
  ASSERT_NE(position,std::string::npos);
  body.replace(position,original_record.size(),encode_state(forged));
  EXPECT_THROW(decode_snapshot(identity,genesis,reseal(body)),std::invalid_argument);
}
TEST(SnapshotCodec, RejectsForgedReceiptsEvenWithValidRecordAndImageChecksums) {
  auto snapshot = ledger_snapshot(); const auto image = snapshot_image();
  const auto original = snapshot.receipts.at(ledger_transaction().id);
  auto forged = original; forged.result = TransferResult::wrong_nonce;
  auto body = image.substr(0,image.size()-32); const auto encoded = encode_receipt(original);
  const auto position = body.find(encoded); ASSERT_NE(position,std::string::npos);
  body.replace(position,encoded.size(),encode_receipt(forged));
  EXPECT_THROW(decode_snapshot(raft_ledger_manifest(),ledger_genesis(),reseal(body)),std::invalid_argument);
  snapshot.receipts.at(original.transaction_id) = forged;
  EXPECT_THROW(encode_snapshot(raft_ledger_manifest(),ledger_genesis(),snapshot),std::invalid_argument);
  snapshot = ledger_snapshot(); snapshot.prefix.voted_for = 2;
  EXPECT_THROW(encode_snapshot(raft_ledger_manifest(),ledger_genesis(),snapshot),std::invalid_argument);
}
TEST(SnapshotChunks, DuplicatesResumeAndGenerationReplacementAreBoundedAndDeterministic) {
  const auto image = snapshot_image(); const auto first = snapshot_chunk(image,0,100);
  auto stage = append_snapshot_chunk({},first);
  EXPECT_EQ(stage.data,first.data); EXPECT_FALSE(stage.complete());
  EXPECT_EQ(append_snapshot_chunk(stage,first).data,stage.data);
  EXPECT_EQ(decode_snapshot_stage(encode_snapshot_stage(stage)).data,stage.data);
  const auto last = snapshot_chunk(image,100,image.size()-100);
  const auto complete = append_snapshot_chunk(stage,last);
  EXPECT_TRUE(complete.complete()); EXPECT_EQ(complete.data,image);
  EXPECT_EQ(append_snapshot_chunk(complete,last).data,image);
  auto other = first; other.id.back() ^= 1;
  EXPECT_EQ(append_snapshot_chunk(stage,other).id,other.id);
  other.offset = 100;
  EXPECT_THROW(append_snapshot_chunk(stage,other),std::invalid_argument);
}
TEST(SnapshotChunks, InvalidOffsetsOverlapSizesAndDigestCannotAdvanceStaging) {
  const auto image = snapshot_image(); const auto first = snapshot_chunk(image,0,100);
  const auto stage = append_snapshot_chunk({},first);
  EXPECT_THROW(append_snapshot_chunk({},snapshot_chunk(image,100,10)),std::invalid_argument);
  EXPECT_THROW(append_snapshot_chunk(stage,snapshot_chunk(image,101,10)),std::invalid_argument);
  EXPECT_THROW(append_snapshot_chunk(stage,snapshot_chunk(image,90,20)),std::invalid_argument);
  auto changed = first; changed.data[10] ^= 1;
  EXPECT_THROW(append_snapshot_chunk(stage,changed),std::invalid_argument);
  changed = first; ++changed.total_bytes;
  EXPECT_THROW(append_snapshot_chunk(stage,changed),std::invalid_argument);
  changed = first; changed.total_bytes = kMaxSnapshotBytes+1;
  EXPECT_THROW(append_snapshot_chunk({},changed),std::invalid_argument);
  changed = first; changed.data = std::string(kMaxSnapshotChunkBytes+1,'x');
  EXPECT_THROW(append_snapshot_chunk({},changed),std::invalid_argument);
  changed = snapshot_chunk(image,100,image.size()-100); changed.data.back() ^= 1;
  EXPECT_THROW(append_snapshot_chunk(stage,changed),std::invalid_argument);
  EXPECT_EQ(stage.data,first.data);
}
} // namespace
