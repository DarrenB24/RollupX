#pragma once
#include "raft_store_fixture.hpp"

namespace rollupx::test {
inline raft::LedgerSnapshot ledger_snapshot(raft::State prefix) {
  raft::LedgerSnapshot snapshot{prefix,{0,ledger_genesis().accounts},{}};
  for (raft::Index index = 1; index <= prefix.commit; ++index) {
    const auto plan = plan_committed_entry(ledger_genesis(),snapshot.ledger,prefix,index,
      [&](const Hash& id) -> std::optional<LedgerReceipt> {
        const auto found = snapshot.receipts.find(id);
        return found == snapshot.receipts.end() ? std::nullopt : std::optional{found->second};
      });
    snapshot.ledger = plan.state;
    for (const auto& receipt : plan.new_receipts) snapshot.receipts.emplace(receipt.transaction_id,receipt);
  }
  return snapshot;
}
inline raft::LedgerSnapshot ledger_snapshot() {
  // Signing is randomized. Preserve exact signed log bytes across fixture calls.
  static const auto snapshot = ledger_snapshot(raft_ledger_history());
  return snapshot;
}
inline std::string snapshot_image() {
  static const auto image = raft::encode_snapshot(raft_ledger_manifest(),ledger_genesis(),ledger_snapshot());
  return image;
}
inline raft::SnapshotChunk snapshot_chunk(const std::string& image, std::size_t offset, std::size_t count) {
  return {raft::snapshot_hash(image),image.size(),offset,image.substr(offset,count)};
}
inline void stage_image(raft::Store& store, const std::string& image) {
  const auto progress = store.staged_snapshot();
  const auto start = progress && progress->id == raft::snapshot_hash(image) ? progress->next_offset : 0;
  for (std::size_t offset = start; offset < image.size(); offset += raft::kMaxSnapshotChunkBytes)
    (void)store.stage_snapshot(snapshot_chunk(image,offset,raft::kMaxSnapshotChunkBytes));
}
} // namespace rollupx::test
