#include "rollupx/raft_snapshot.hpp"
#include <algorithm>

namespace rollupx::raft {
namespace {
constexpr std::size_t kMaxReceipts = kMaxLogBytes/kTransactionBytes;
std::span<const Byte> bytes(std::string_view value) {
  return {reinterpret_cast<const Byte*>(value.data()),value.size()};
}
void integer(std::string& out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) out.push_back(static_cast<char>(value >> (8*(width-i-1))));
}
std::string seal(std::string out) {
  const auto hash = snapshot_hash(out); out.append(reinterpret_cast<const char*>(hash.data()),hash.size()); return out;
}
void record(std::string& out, const std::string& value, std::size_t limit = kMaxSnapshotBytes) {
  if (value.size() > limit-4 || out.size() > limit-value.size()-4)
    throw std::invalid_argument("snapshot record capacity exceeded");
  integer(out,value.size(),4); out += value;
}
class Reader {
 public:
  Reader(std::string_view input, std::string_view magic, std::size_t limit) {
    if (input.size() < magic.size()+32 || input.size() > limit || !input.starts_with(magic))
      throw std::invalid_argument("invalid snapshot size/version");
    const auto payload = input.substr(0,input.size()-32);
    const auto hash = snapshot_hash(payload);
    if (!std::equal(hash.begin(),hash.end(),bytes(input).end()-32)) throw std::invalid_argument("snapshot checksum mismatch");
    remaining = payload.substr(magic.size());
  }
  std::string_view take(std::size_t count) {
    if (count > remaining.size()) throw std::invalid_argument("truncated snapshot");
    const auto result = remaining.substr(0,count); remaining.remove_prefix(count); return result;
  }
  std::uint64_t number(std::size_t width) {
    std::uint64_t result = 0; for (const auto value : bytes(take(width))) result = (result << 8) | value; return result;
  }
  std::string_view record() { return take(number(4)); }
  void finish() const { if (!remaining.empty()) throw std::invalid_argument("trailing snapshot bytes"); }
 private:
  std::string_view remaining;
};
Manifest portable(Manifest manifest, const Genesis& genesis) {
  validate_ledger_identity(manifest,genesis);
  // The record binds the entire configuration/genesis, but is portable to another
  // member. Never copy the source node's persistent term or vote to the target.
  manifest.node_id = manifest.members.front(); return manifest;
}
LedgerSnapshot replay(const Manifest& manifest, const Genesis& genesis, const State& prefix) {
  validate_ledger_identity(manifest,genesis); validate_state(prefix,manifest.members);
  if (prefix.log.empty() || prefix.commit != prefix.log.size() || prefix.voted_for || prefix.term != prefix.log.back().term)
    throw std::invalid_argument("snapshot must describe a nonempty committed prefix without election metadata");
  LedgerSnapshot result{prefix,{0,genesis.accounts},{}};
  for (Index index = 1; index <= prefix.commit; ++index) {
    const auto plan = plan_committed_entry(genesis,result.ledger,prefix,index,[&](const Hash& id) -> std::optional<LedgerReceipt> {
      const auto found = result.receipts.find(id);
      return found == result.receipts.end() ? std::nullopt : std::optional{found->second};
    });
    for (const auto& receipt : plan.new_receipts) result.receipts.emplace(receipt.transaction_id,receipt);
    result.ledger = plan.state;
  }
  return result;
}
void validate_stage(const SnapshotStage& stage) {
  if (stage.id == Hash{} || stage.total_bytes < 36 || stage.total_bytes > kMaxSnapshotBytes ||
      stage.data.empty() || stage.data.size() > stage.total_bytes)
    throw std::invalid_argument("invalid snapshot staging bounds");
  if (stage.complete() && snapshot_hash(stage.data) != stage.id) throw std::invalid_argument("snapshot transfer digest mismatch");
}
} // namespace
Hash snapshot_hash(std::string_view value) { return keccak256(bytes(value)); }
LedgerSnapshot make_snapshot(const Manifest& manifest, const Genesis& genesis, const State& consensus,
    const LedgerState& ledger, const ReceiptLookup& lookup) {
  validate_state(consensus,manifest.members);
  if (!lookup || consensus.commit == 0 || ledger.applied_index != consensus.commit)
    throw std::invalid_argument("snapshot requires fully applied committed progress");
  State prefix = consensus; prefix.log.resize(consensus.commit); prefix.term = prefix.log.back().term; prefix.voted_for.reset();
  auto result = replay(manifest,genesis,prefix);
  if (result.ledger != ledger) throw std::runtime_error("snapshot accounts differ from committed replay");
  for (const auto& [id,expected] : result.receipts) {
    const auto actual = lookup(id);
    if (!actual || *actual != expected) throw std::runtime_error("snapshot receipt differs from committed replay");
  }
  return result;
}
std::string encode_snapshot(const Manifest& manifest, const Genesis& genesis, const LedgerSnapshot& snapshot) {
  if (snapshot.receipts.size() > kMaxReceipts || replay(manifest,genesis,snapshot.prefix) != snapshot)
    throw std::invalid_argument("snapshot state/receipts do not match signed history");
  std::string out = "RXS1";
  record(out,encode_state_record(portable(manifest,genesis),snapshot.prefix));
  record(out,encode_state(snapshot.ledger)); integer(out,snapshot.receipts.size(),4);
  for (const auto& [id,receipt] : snapshot.receipts) {
    if (id != receipt.transaction_id) throw std::invalid_argument("snapshot receipt key mismatch");
    record(out,encode_receipt(receipt));
  }
  if (out.size() > kMaxSnapshotBytes-32) throw std::invalid_argument("snapshot capacity exceeded");
  return seal(std::move(out));
}
LedgerSnapshot decode_snapshot(const Manifest& manifest, const Genesis& genesis, std::string_view input) {
  Reader reader(input,"RXS1",kMaxSnapshotBytes);
  LedgerSnapshot result;
  result.prefix = decode_state_record(portable(manifest,genesis),reader.record());
  result.ledger = decode_state(reader.record());
  const auto count = reader.number(4);
  if (count > kMaxReceipts) throw std::invalid_argument("snapshot receipt count exceeds capacity");
  std::optional<Hash> previous;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto receipt = decode_receipt(reader.record());
    if (previous && receipt.transaction_id <= *previous) throw std::invalid_argument("snapshot receipts must be unique and sorted");
    result.receipts.emplace(receipt.transaction_id,receipt); previous = receipt.transaction_id;
  }
  reader.finish();
  if (replay(manifest,genesis,result.prefix) != result) throw std::invalid_argument("snapshot replay mismatch");
  return result;
}
std::string encode_snapshot_stage(const SnapshotStage& stage) {
  validate_stage(stage); std::string out = "RXSG1";
  out.append(reinterpret_cast<const char*>(stage.id.data()),stage.id.size());
  integer(out,stage.total_bytes,8); record(out,stage.data,kMaxSnapshotBytes+81); return seal(std::move(out));
}
SnapshotStage decode_snapshot_stage(std::string_view input) {
  Reader reader(input,"RXSG1",kMaxSnapshotBytes+81); SnapshotStage result{};
  const auto id = bytes(reader.take(32)); std::copy(id.begin(),id.end(),result.id.begin());
  result.total_bytes = reader.number(8); result.data = reader.record(); reader.finish(); validate_stage(result); return result;
}
SnapshotStage append_snapshot_chunk(const std::optional<SnapshotStage>& current, const SnapshotChunk& chunk) {
  if (chunk.id == Hash{} || chunk.total_bytes < 36 || chunk.total_bytes > kMaxSnapshotBytes || chunk.data.empty() ||
      chunk.data.size() > kMaxSnapshotChunkBytes || chunk.offset > chunk.total_bytes || chunk.data.size() > chunk.total_bytes-chunk.offset)
    throw std::invalid_argument("invalid snapshot chunk bounds");
  SnapshotStage next = current.value_or(SnapshotStage{chunk.id,chunk.total_bytes,{}});
  if (next.id != chunk.id) {
    if (chunk.offset != 0) throw std::invalid_argument("new snapshot must start at offset zero");
    next = {chunk.id,chunk.total_bytes,{}};
  }
  if (next.total_bytes != chunk.total_bytes || chunk.offset > next.data.size())
    throw std::invalid_argument("snapshot length changed or chunk has a hole");
  if (chunk.offset < next.data.size()) {
    if (chunk.data.size() > next.data.size()-chunk.offset || next.data.compare(chunk.offset,chunk.data.size(),chunk.data) != 0)
      throw std::invalid_argument("snapshot retry changed or overlaps staged bytes");
  } else next.data += chunk.data;
  validate_stage(next); return next;
}
} // namespace rollupx::raft
