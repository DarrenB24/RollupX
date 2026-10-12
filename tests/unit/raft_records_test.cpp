#include "../raft_store_fixture.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace rollupx;
using namespace rollupx::raft;
using namespace rollupx::test;
std::string reseal(std::string encoded) {
  encoded.resize(encoded.size()-32);
  const auto hash = keccak256({reinterpret_cast<const Byte*>(encoded.data()),encoded.size()});
  encoded.append(reinterpret_cast<const char*>(hash.data()),hash.size()); return encoded;
}
TEST(RaftRecords, RoundTripsManifestStateNoopAndBinaryCommands) {
  const auto identity = raft_manifest();
  EXPECT_EQ(decode_manifest(encode_manifest(identity)),identity);
  for (const auto& state : {State{},State{7,3,{{2,{}},{7,std::string("a\0b",3)}},1}})
    EXPECT_EQ(decode_state_record(identity,encode_state_record(identity,state)),state);
  const State maximum_term{std::numeric_limits<Term>::max(),{}, {},0};
  EXPECT_EQ(decode_state_record(identity,encode_state_record(identity,maximum_term)),maximum_term);
}
TEST(RaftRecords, FreezesVersionOffsetsAndBigEndianFields) {
  const auto identity = raft_manifest(); const State state{0x0102030405060708ULL,3,{{1,{}}},1};
  const auto encoded = encode_state_record(identity,state);
  EXPECT_EQ(encoded.substr(0,4),"RXF1"); EXPECT_EQ(encoded.size(),104U);
  const auto body = std::string_view(encoded).substr(36,36);
  EXPECT_EQ(hex({reinterpret_cast<const Byte*>(body.data()),body.size()}),
    "010203040506070800000003000000000000000100000001000000000000000100000000");
}
TEST(RaftRecords, EveryChangedByteTruncationWrongVersionAndTrailingByteFails) {
  const auto identity = raft_manifest(); const auto manifest = encode_manifest(identity);
  const auto state = encode_state_record(identity,{2,1,{{2,"binary"}},1});
  for (std::size_t i = 0; i < manifest.size(); ++i) {
    auto changed = manifest; changed[i] ^= 1; EXPECT_THROW(decode_manifest(changed),std::invalid_argument);
  }
  for (std::size_t i = 0; i < state.size(); ++i) {
    auto changed = state; changed[i] ^= 1; EXPECT_THROW(decode_state_record(identity,changed),std::invalid_argument);
    EXPECT_THROW(decode_state_record(identity,std::string_view(state).substr(0,i)),std::invalid_argument);
  }
  EXPECT_THROW(decode_state_record(identity,state+"x"),std::invalid_argument);
  EXPECT_THROW(decode_manifest(state),std::invalid_argument);
  EXPECT_THROW(decode_state_record(identity,manifest),std::invalid_argument);
}
TEST(RaftRecords, StateBindsNodeClusterMembershipAndGenesis) {
  const auto identity = raft_manifest(); const auto encoded = encode_state_record(identity,{});
  auto changed = identity; changed.node_id = 2;
  EXPECT_THROW(decode_state_record(changed,encoded),std::invalid_argument);
  changed = identity; changed.cluster_id = "different";
  EXPECT_THROW(decode_state_record(changed,encoded),std::invalid_argument);
  changed = identity; changed.members = {1,2,3};
  EXPECT_THROW(decode_state_record(changed,encoded),std::invalid_argument);
  changed = identity; changed.genesis_digest[0] ^= 1;
  EXPECT_THROW(decode_state_record(changed,encoded),std::invalid_argument);
}
TEST(RaftRecords, ManifestRejectsInvalidMembershipAndIdentity) {
  const auto identity = raft_manifest();
  auto bad = identity; bad.members = {1,3,2}; EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.members = {1,2,2}; EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.node_id = 99; EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.genesis_digest = {}; EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  for (const auto& name : {std::string{},std::string(129,'a'),std::string("bad/name")}) {
    bad = identity; bad.cluster_id = name; EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  }
}
TEST(RaftRecords, ValidChecksumCannotBypassStructuralBoundsOrSemanticChecks) {
  const auto identity = raft_manifest(); const auto encoded = encode_state_record(identity,{2,1,{{2,"x"}},1});
  auto changed = encoded; changed[56] = '\x7f'; // Unbounded entry count.
  EXPECT_THROW(decode_state_record(identity,reseal(changed)),std::invalid_argument);
  changed = encoded; changed[68] = '\x7f'; // Unbounded command length.
  EXPECT_THROW(decode_state_record(identity,reseal(changed)),std::invalid_argument);
  changed = encoded; changed[55] = 2; // Commit exceeds last log index.
  EXPECT_THROW(decode_state_record(identity,reseal(changed)),std::invalid_argument);
  changed = encoded; changed[47] = 99; // Non-member vote.
  EXPECT_THROW(decode_state_record(identity,reseal(changed)),std::invalid_argument);
  changed = encoded; changed.insert(changed.size()-32,1,'x');
  EXPECT_THROW(decode_state_record(identity,reseal(changed)),std::invalid_argument);
  EXPECT_THROW(decode_state_record(identity,std::string(kMaxStateRecordBytes+1,'x')),std::invalid_argument);
}
TEST(RaftRecords, RejectsRegressionsVoteChangesAndCommittedHistoryChanges) {
  const auto identity = raft_manifest(); const State before{3,2,{{1,"committed"},{2,"tail"}},1};
  auto next = before; next.term = 2;
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.commit = 0;
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.voted_for.reset();
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.voted_for = 3;
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.log[0].command = "changed";
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.log[1].command = "same-term-change";
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
  next = before; next.log.pop_back();
  EXPECT_THROW(validate_persisted_transition(identity,before,next),std::invalid_argument);
}
TEST(RaftRecords, AllowsLegalReelectionConflictReplacementAndCommitAdvancement) {
  const auto identity = raft_manifest(); const State before{3,2,{{1,"committed"},{2,"tail"},{2,"extra"}},1};
  auto next = before; next.term = 4; next.voted_for.reset();
  EXPECT_NO_THROW(validate_persisted_transition(identity,before,next));
  next = before; next.log.resize(2); next.log[1] = {3,"replacement"}; next.commit = 2;
  EXPECT_NO_THROW(validate_persisted_transition(identity,before,next));
  EXPECT_NO_THROW(validate_persisted_transition(identity,next,next));
}
TEST(RaftRecords, ProtocolPersistsRoundTrippableRecordsAndRestartsWithoutRevoting) {
  const auto identity = raft_manifest(); std::string record = encode_state_record(identity,{});
  auto persist = [&](const State& next) {
    validate_persisted_transition(identity,decode_state_record(identity,record),next);
    record = encode_state_record(identity,next);
  };
  {
    Core core(raft_config(),decode_state_record(identity,record),persist);
    (void)core.receive({2,1,VoteRequest{1,0,0}},0);
    (void)core.receive({2,1,AppendRequest{1,1,0,0,{{1,"command"}},1}},1);
  }
  Core recovered(raft_config(),decode_state_record(identity,record),persist);
  const auto response = recovered.receive({3,1,VoteRequest{1,1,1}},2);
  EXPECT_FALSE(std::get<VoteResponse>(response[0].message).granted);
  EXPECT_EQ(recovered.state().commit,1U); EXPECT_EQ(recovered.state().log[0].command,"command");
}
Manifest named_identity() {
  return make_named_manifest("test-cluster",{"n1","n2","n3","n4","n5"},raft_manifest().genesis_digest,1);
}
TEST(RaftRecords, NamedManifestRoundTripsAndBindsPersistedVotesToNames) {
  const auto identity = named_identity(); const State voted{2,2,{{2,"kept"}},1};
  const auto record = encode_manifest(identity);
  EXPECT_EQ(record.substr(0,4),"RXM2"); EXPECT_EQ(decode_manifest(record),identity);
  const auto state = encode_state_record(identity,voted);
  EXPECT_EQ(decode_state_record(decode_manifest(record),state),voted);
  auto renamed = identity; renamed.member_names[1] = "n2-renamed";
  EXPECT_THROW(decode_state_record(renamed,state),std::invalid_argument);
  auto legacy = identity; legacy.member_names.clear();
  EXPECT_THROW(decode_state_record(legacy,state),std::invalid_argument);
  EXPECT_THROW(decode_state_record(identity,encode_state_record(legacy,voted)),std::invalid_argument);
}
TEST(RaftRecords, NamedMembershipRejectsUnsortedDuplicateAndNonpositionalIds) {
  const auto identity = named_identity();
  auto bad = identity; std::swap(bad.member_names[0],bad.member_names[1]);
  EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.member_names[1] = bad.member_names[0];
  EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.member_names.pop_back();
  EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  bad = identity; bad.members.back() = 99;
  EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  for (const auto& name : {std::string{},std::string("bad/name"),std::string(129,'z')}) {
    bad = identity; bad.member_names.back() = name;
    EXPECT_THROW(encode_manifest(bad),std::invalid_argument);
  }
}
TEST(RaftRecords, NamedManifestHasBoundedMaximumAndRejectsEveryTruncatedPrefix) {
  std::vector<std::string> names;
  for (int i = 0; i < 31; ++i) names.push_back(std::string(127,'a')+static_cast<char>('A'+i));
  // Restrict suffixes to identifier characters across the ASCII punctuation gap.
  for (int i = 26; i < 31; ++i) names[i].back() = static_cast<char>('a'+i-26);
  const auto maximum = make_named_manifest(std::string(128,'c'),names,raft_manifest().genesis_digest,31);
  const auto encoded = encode_manifest(maximum);
  EXPECT_EQ(encoded.size(),kMaxManifestBytes); EXPECT_EQ(decode_manifest(encoded),maximum);
  const auto small = encode_manifest(named_identity());
  for (std::size_t size = 0; size < small.size(); ++size)
    EXPECT_THROW(decode_manifest(std::string_view(small).substr(0,size)),std::invalid_argument);
  EXPECT_THROW(decode_manifest(encoded+"x"),std::invalid_argument);
}
TEST(RaftRecords, RecomputedChecksumCannotHideInvalidNameEncoding) {
  const auto encoded = encode_manifest(named_identity());
  const std::size_t names_at = 4+2+12+4+1+5*4;
  ASSERT_EQ(static_cast<unsigned char>(encoded[names_at]),2U);
  auto bad = encoded; bad[names_at] = 0;
  EXPECT_THROW(decode_manifest(reseal(bad)),std::invalid_argument);
  bad = encoded; bad[names_at] = static_cast<char>(129);
  EXPECT_THROW(decode_manifest(reseal(bad)),std::invalid_argument);
  bad = encoded; bad[names_at+1] = '/';
  EXPECT_THROW(decode_manifest(reseal(bad)),std::invalid_argument);
  bad = encoded; bad[names_at+5] = '1'; // Second name duplicates first.
  EXPECT_THROW(decode_manifest(reseal(bad)),std::invalid_argument);
  bad = encoded; bad[3] = '1'; // Format downgrade leaves unexplained name bytes.
  EXPECT_THROW(decode_manifest(reseal(bad)),std::invalid_argument);
}
TEST(RaftRecords, ConfigurationDigestIsSharedAcrossNodesAndRejectsLegacyIdentity) {
  const auto identity = named_identity(); const auto expected = configuration_digest(identity);
  std::string frozen = "ROLLUPX_RAFT_WIRE_V1" "\x0c" "test-cluster" "\x05"
    "\x02" "n1" "\x02" "n2" "\x02" "n3" "\x02" "n4" "\x02" "n5";
  frozen.append(31,'\0'); frozen.push_back('\x01');
  EXPECT_EQ(expected,keccak256({reinterpret_cast<const Byte*>(frozen.data()),frozen.size()}));
  auto changed = identity; changed.node_id = 2;
  EXPECT_EQ(configuration_digest(changed),expected);
  changed.member_names.back() = "n6"; EXPECT_NE(configuration_digest(changed),expected);
  changed = identity; changed.cluster_id = "other"; EXPECT_NE(configuration_digest(changed),expected);
  changed = identity; changed.genesis_digest[0] ^= 1; EXPECT_NE(configuration_digest(changed),expected);
  EXPECT_THROW(configuration_digest(raft_manifest()),std::invalid_argument);
}
TEST(RaftRecords, LegacyEncodingRetainsOriginalBytesAndCannotAcquireImplicitNames) {
  const auto legacy = raft_manifest(); const auto encoded = encode_manifest(legacy);
  EXPECT_EQ(encoded.substr(0,4),"RXM1"); EXPECT_EQ(encoded.size(),107U);
  EXPECT_TRUE(decode_manifest(encoded).member_names.empty());
  EXPECT_EQ(encode_manifest(decode_manifest(encoded)),encoded);
  EXPECT_THROW(make_named_manifest("test",{},legacy.genesis_digest,1),std::invalid_argument);
}
} // namespace
