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
} // namespace
