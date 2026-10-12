#include "rollupx/raft_records.hpp"
#include <algorithm>
#include <utility>

namespace rollupx::raft {
namespace {
std::span<const Byte> bytes(std::string_view input) {
  return {reinterpret_cast<const Byte*>(input.data()),input.size()};
}
void integer(std::string& out, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) out.push_back(static_cast<char>(value >> (8*(width-i-1))));
}
void append(std::string& out, std::span<const Byte> input) {
  out.append(reinterpret_cast<const char*>(input.data()),input.size());
}
std::string seal(std::string out) { const auto checksum = keccak256(bytes(out)); append(out,checksum); return out; }
class Reader {
 public:
  Reader(std::string_view input, std::string_view magic, std::size_t limit) {
    if (input.size() < magic.size()+32 || input.size() > limit || !input.starts_with(magic))
      throw std::invalid_argument("invalid Raft record size or version");
    const auto payload = input.substr(0,input.size()-32); const auto hash = keccak256(bytes(payload));
    if (!std::equal(hash.begin(),hash.end(),bytes(input).end()-32)) throw std::invalid_argument("Raft record checksum mismatch");
    remaining = payload.substr(magic.size());
  }
  std::string_view take(std::size_t size) {
    if (size > remaining.size()) throw std::invalid_argument("truncated Raft record");
    const auto result = remaining.substr(0,size); remaining.remove_prefix(size); return result;
  }
  std::uint64_t number(std::size_t width) {
    std::uint64_t result = 0; for (const auto value : bytes(take(width))) result = (result << 8) | value; return result;
  }
  void finish() const { if (!remaining.empty()) throw std::invalid_argument("trailing Raft record data"); }
 private:
  std::string_view remaining;
};
Hash manifest_hash(const Manifest& manifest) { return keccak256(bytes(encode_manifest(manifest))); }
void identifier(std::string_view value) {
  if (value.empty() || value.size() > 128) throw std::invalid_argument("invalid Raft identifier length");
  for (const auto c : value)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      throw std::invalid_argument("invalid Raft identifier character");
}
} // namespace
void validate_manifest(const Manifest& manifest) {
  validate_membership(manifest.node_id,manifest.members);
  identifier(manifest.cluster_id);
  if (manifest.genesis_digest == Hash{})
    throw std::invalid_argument("invalid Raft cluster/genesis identity");
  if (!manifest.member_names.empty()) {
    const auto& names = manifest.member_names;
    if (names.size() != manifest.members.size() || !std::is_sorted(names.begin(),names.end()) ||
        std::adjacent_find(names.begin(),names.end()) != names.end())
      throw std::invalid_argument("invalid named Raft membership");
    for (std::size_t i = 0; i < names.size(); ++i) {
      identifier(names[i]);
      if (manifest.members[i] != i+1) throw std::invalid_argument("named Raft IDs must match name positions");
    }
  }
}
Manifest make_named_manifest(std::string cluster, std::vector<std::string> members, Hash genesis, NodeId local) {
  if (members.size() < 3 || members.size() > 31) throw std::invalid_argument("invalid named Raft member count");
  Manifest result{std::move(cluster),local,{},genesis,std::move(members)};
  for (NodeId i = 1; i <= result.member_names.size(); ++i) result.members.push_back(i);
  validate_manifest(result); return result;
}
Hash configuration_digest(const Manifest& manifest) {
  validate_manifest(manifest);
  if (manifest.member_names.empty()) throw std::invalid_argument("wire configuration requires named Raft membership");
  std::string encoded = "ROLLUPX_RAFT_WIRE_V1";
  integer(encoded,manifest.cluster_id.size(),1); encoded += manifest.cluster_id;
  integer(encoded,manifest.member_names.size(),1);
  for (const auto& name : manifest.member_names) { integer(encoded,name.size(),1); encoded += name; }
  append(encoded,manifest.genesis_digest); return keccak256(bytes(encoded));
}
std::string encode_manifest(const Manifest& manifest) {
  validate_manifest(manifest); std::string out = manifest.member_names.empty() ? "RXM1" : "RXM2";
  integer(out,manifest.cluster_id.size(),2); out += manifest.cluster_id;
  integer(out,manifest.node_id,4); integer(out,manifest.members.size(),1);
  for (const auto member : manifest.members) integer(out,member,4);
  for (const auto& name : manifest.member_names) { integer(out,name.size(),1); out += name; }
  append(out,manifest.genesis_digest); return seal(std::move(out));
}
Manifest decode_manifest(std::string_view input) {
  const bool named = input.starts_with("RXM2");
  Reader reader(input,named ? "RXM2" : "RXM1",named ? kMaxManifestBytes : 512); Manifest result{};
  const auto length = reader.number(2);
  if (length == 0 || length > 128) throw std::invalid_argument("Raft cluster name length out of bounds");
  result.cluster_id = reader.take(length); result.node_id = static_cast<NodeId>(reader.number(4));
  const auto count = reader.number(1);
  if (count < 3 || count > 31 || count%2 == 0) throw std::invalid_argument("Raft membership count out of bounds");
  for (std::uint64_t i = 0; i < count; ++i) result.members.push_back(static_cast<NodeId>(reader.number(4)));
  if (named) for (std::uint64_t i = 0; i < count; ++i) {
    const auto name_length = reader.number(1);
    if (name_length == 0 || name_length > 128) throw std::invalid_argument("Raft member name length out of bounds");
    result.member_names.emplace_back(reader.take(name_length));
  }
  const auto hash = bytes(reader.take(32)); std::copy(hash.begin(),hash.end(),result.genesis_digest.begin());
  reader.finish(); validate_manifest(result); return result;
}
std::string encode_state_record(const Manifest& manifest, const State& state) {
  validate_manifest(manifest); validate_state(state,manifest.members);
  std::string out = "RXF1"; append(out,manifest_hash(manifest));
  integer(out,state.term,8); integer(out,state.voted_for.value_or(0),4);
  integer(out,state.commit,8); integer(out,state.log.size(),4);
  for (const auto& entry : state.log) {
    integer(out,entry.term,8); integer(out,entry.command.size(),4); out += entry.command;
  }
  return seal(std::move(out));
}
State decode_state_record(const Manifest& manifest, std::string_view input) {
  validate_manifest(manifest); Reader reader(input,"RXF1",kMaxStateRecordBytes);
  const auto expected = manifest_hash(manifest); const auto actual = bytes(reader.take(32));
  if (!std::equal(expected.begin(),expected.end(),actual.begin())) throw std::invalid_argument("Raft state belongs to another identity");
  State result{}; result.term = reader.number(8); const auto vote = reader.number(4);
  if (vote != 0) result.voted_for = static_cast<NodeId>(vote);
  result.commit = reader.number(8); const auto count = reader.number(4);
  if (count > kMaxLogEntries) throw std::invalid_argument("Raft log count out of bounds");
  std::size_t total = 0; result.log.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto term = reader.number(8), length = reader.number(4);
    if (length > kMaxCommandBytes || length > kMaxLogBytes-total) throw std::invalid_argument("Raft command data exceeds bounds");
    result.log.push_back({term,std::string(reader.take(length))}); total += length;
  }
  reader.finish(); validate_state(result,manifest.members); return result;
}
void validate_persisted_transition(const Manifest& manifest, const State& before, const State& after) {
  validate_manifest(manifest); validate_state(before,manifest.members); validate_state(after,manifest.members);
  if (after.term < before.term || after.commit < before.commit ||
      (after.term == before.term && before.voted_for && after.voted_for != before.voted_for))
    throw std::invalid_argument("Raft term/commit regression or same-term vote change");
  bool replaced = false;
  for (std::size_t i = 0; i < std::min(before.log.size(),after.log.size()); ++i) {
    if (before.log[i] == after.log[i]) continue;
    if (i < before.commit || before.log[i].term == after.log[i].term)
      throw std::invalid_argument("Raft committed-prefix or same-term command change");
    replaced = true;
  }
  if (after.log.size() < before.log.size() && !replaced)
    throw std::invalid_argument("Raft suffix deletion without conflicting replacement is unsupported");
}
} // namespace rollupx::raft
