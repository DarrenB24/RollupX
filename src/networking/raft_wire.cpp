#include "rollupx/raft_wire.hpp"
#include <algorithm>
#include <google/protobuf/unknown_field_set.h>

namespace rollupx::raft {
namespace {
namespace pb = internal::v1;
void supported(const google::protobuf::Message& message, std::size_t limit) {
  if (message.ByteSizeLong() > limit || !message.GetReflection()->GetUnknownFields(message).empty())
    throw std::invalid_argument("oversized or unsupported Raft protobuf message");
}
Term request_term(const Envelope& envelope) {
  if (const auto* vote = std::get_if<VoteRequest>(&envelope.message)) return vote->term;
  if (const auto* append = std::get_if<AppendRequest>(&envelope.message)) return append->term;
  throw std::invalid_argument("expected Raft request");
}
std::uint64_t rpc(const Envelope& envelope) {
  return std::holds_alternative<VoteRequest>(envelope.message) ? 0 : std::get<AppendRequest>(envelope.message).rpc;
}
void validate_request(const Envelope& envelope) {
  const auto term = request_term(envelope);
  if (term == 0) throw std::invalid_argument("zero Raft request term");
  if (const auto* vote = std::get_if<VoteRequest>(&envelope.message)) {
    if (vote->last_index > kMaxLogEntries || vote->last_term > term || (vote->last_index == 0) != (vote->last_term == 0))
      throw std::invalid_argument("invalid vote log position");
    return;
  }
  const auto& append = std::get<AppendRequest>(envelope.message);
  if (append.rpc == 0 || append.previous_index > kMaxLogEntries || append.leader_commit > kMaxLogEntries ||
      append.entries.size() > kMaxAppendEntries || append.entries.size() > kMaxLogEntries-append.previous_index ||
      append.previous_term > term || (append.previous_index == 0) != (append.previous_term == 0))
    throw std::invalid_argument("invalid append bounds or previous position");
  std::size_t total = 0; Term previous = append.previous_term;
  for (const auto& entry : append.entries) {
    if (entry.term == 0 || entry.term < previous || entry.term > term || entry.command.size() > kMaxCommandBytes)
      throw std::invalid_argument("invalid append entry");
    total += entry.command.size(); previous = entry.term;
  }
  if (total > kMaxAppendBytes) throw std::invalid_argument("append commands exceed wire budget");
}
void validate_response(const Envelope& response, const Envelope& request) {
  validate_request(request);
  if (response.from != request.to || response.to != request.from)
    throw std::invalid_argument("Raft response route mismatch");
  const auto term = request_term(request);
  if (std::holds_alternative<VoteRequest>(request.message)) {
    const auto* vote = std::get_if<VoteResponse>(&response.message);
    if (!vote || vote->term < term || (vote->granted && vote->term != term))
      throw std::invalid_argument("invalid vote response term/type");
  } else {
    const auto& call = std::get<AppendRequest>(request.message);
    const auto* append = std::get_if<AppendResponse>(&response.message);
    if (!append || append->term < term || append->rpc != call.rpc)
      throw std::invalid_argument("invalid append response correlation");
    if (append->success) {
      if (append->term != term || append->match_index != call.previous_index+call.entries.size() || append->conflict_index != 0)
        throw std::invalid_argument("invalid successful append response");
    } else if (append->match_index != 0 || append->conflict_index == 0 || append->conflict_index > kMaxLogEntries+1 ||
        (append->term == term && append->conflict_index > call.previous_index)) {
      throw std::invalid_argument("invalid append conflict hint");
    }
  }
}
template<class T> T parse(std::string_view bytes, std::size_t limit) {
  if (bytes.empty() || bytes.size() > limit) throw std::invalid_argument("invalid Raft wire size");
  T message;
  if (!message.ParseFromArray(bytes.data(),static_cast<int>(bytes.size()))) throw std::invalid_argument("malformed Raft protobuf");
  return message;
}
template<class T> std::string serialize(const T& variant) {
  return std::visit([](const auto& message) {
    supported(message,kMaxAppendWireBytes);
    std::string bytes;
    if (!message.SerializeToString(&bytes)) throw std::runtime_error("Raft protobuf serialization failed");
    return bytes;
  },variant);
}
} // namespace
WireCodec::WireCodec(std::string cluster, std::vector<std::string> members, Hash genesis, NodeId local)
  : WireCodec(make_named_manifest(std::move(cluster),std::move(members),genesis,local)) {}
WireCodec::WireCodec(Manifest identity)
  : identity_(std::move(identity)), configuration_(configuration_digest(identity_)) {}
const std::string& WireCodec::name(NodeId value) const {
  if (value == 0 || value > identity_.member_names.size()) throw std::invalid_argument("unknown numeric Raft member");
  return identity_.member_names[value-1];
}
NodeId WireCodec::id(std::string_view value) const {
  const auto& names = identity_.member_names;
  const auto found = std::lower_bound(names.begin(),names.end(),value);
  if (found == names.end() || *found != value) throw std::invalid_argument("unknown named Raft member");
  return static_cast<NodeId>(found-names.begin()+1);
}
void WireCodec::route(const Envelope& message, bool outbound) const {
  (void)name(message.from); (void)name(message.to);
  if (message.from == message.to || (outbound ? message.from : message.to) != identity_.node_id)
    throw std::invalid_argument("Raft message addressed to wrong node");
}
pb::RaftContext WireCodec::context(const Envelope& message, const Envelope& request) const {
  pb::RaftContext result; result.set_protocol_version(1);
  result.set_configuration_id(configuration_.data(),configuration_.size());
  result.set_sender_id(name(message.from)); result.set_recipient_id(name(message.to));
  result.set_request_term(request_term(request)); result.set_rpc_id(rpc(request)); return result;
}
void WireCodec::check_context(const pb::RaftContext& actual, const Envelope& message, const Envelope& request) const {
  supported(actual,kMaxVoteWireBytes);
  const auto expected = context(message,request);
  if (actual.protocol_version() != expected.protocol_version() || actual.configuration_id() != expected.configuration_id() ||
      actual.sender_id() != expected.sender_id() || actual.recipient_id() != expected.recipient_id() ||
      actual.request_term() != expected.request_term() || actual.rpc_id() != expected.rpc_id())
    throw std::invalid_argument("Raft context or request correlation mismatch");
}
WireRequest WireCodec::encode_request(const Envelope& request) const {
  route(request,true); validate_request(request);
  if (const auto* vote = std::get_if<VoteRequest>(&request.message)) {
    pb::RequestVoteRequest result; result.set_cluster_id(identity_.cluster_id); result.set_candidate_id(name(request.from));
    result.set_term(vote->term); result.set_last_log_index(vote->last_index); result.set_last_log_term(vote->last_term);
    *result.mutable_context() = context(request,request); return result;
  }
  const auto& append = std::get<AppendRequest>(request.message);
  pb::AppendEntriesRequest result; result.set_cluster_id(identity_.cluster_id); result.set_leader_id(name(request.from));
  result.set_term(append.term); result.set_prev_log_index(append.previous_index); result.set_prev_log_term(append.previous_term);
  result.set_leader_commit(append.leader_commit); *result.mutable_context() = context(request,request);
  Index index = append.previous_index;
  for (const auto& entry : append.entries) {
    auto* item = result.add_entries(); item->set_index(++index); item->set_term(entry.term); item->set_command(entry.command);
  }
  return result;
}
Envelope WireCodec::decode_request(const WireRequest& request) const {
  return std::visit([&](const auto& message) -> Envelope {
    using T = std::decay_t<decltype(message)>;
    constexpr bool vote = std::is_same_v<T,pb::RequestVoteRequest>;
    supported(message,vote ? kMaxVoteWireBytes : kMaxAppendWireBytes);
    if (!message.has_context() || message.cluster_id() != identity_.cluster_id) throw std::invalid_argument("missing Raft context or wrong cluster");
    const auto& ctx = message.context(); Envelope result{id(ctx.sender_id()),id(ctx.recipient_id()),VoteRequest{}};
    route(result,false);
    if constexpr (vote) {
      if (message.candidate_id() != ctx.sender_id()) throw std::invalid_argument("candidate identity mismatch");
      result.message = VoteRequest{message.term(),message.last_log_index(),message.last_log_term()};
    } else {
      if (message.leader_id() != ctx.sender_id() || message.entries_size() > static_cast<int>(kMaxAppendEntries) ||
          message.prev_log_index() > kMaxLogEntries) throw std::invalid_argument("invalid append identity/count/index");
      AppendRequest append{message.term(),ctx.rpc_id(),message.prev_log_index(),message.prev_log_term(),{},message.leader_commit()};
      Index index = append.previous_index;
      for (const auto& entry : message.entries()) {
        supported(entry,kMaxCommandBytes+32);
        if (entry.index() != ++index) throw std::invalid_argument("noncontiguous Raft entry index");
        append.entries.push_back({entry.term(),entry.command()});
      }
      result.message = std::move(append);
    }
    validate_request(result); check_context(ctx,result,result); return result;
  },request);
}
WireResponse WireCodec::encode_response(const Envelope& response, const Envelope& request) const {
  route(request,false); route(response,true); validate_response(response,request);
  if (const auto* vote = std::get_if<VoteResponse>(&response.message)) {
    pb::RequestVoteResponse result; result.set_term(vote->term); result.set_vote_granted(vote->granted);
    *result.mutable_context() = context(response,request); return result;
  }
  const auto& append = std::get<AppendResponse>(response.message);
  pb::AppendEntriesResponse result; result.set_term(append.term); result.set_success(append.success);
  result.set_match_index(append.match_index); result.set_conflict_index(append.conflict_index);
  *result.mutable_context() = context(response,request); return result;
}
Envelope WireCodec::decode_response(const WireResponse& response, const Envelope& request) const {
  route(request,true); validate_request(request);
  return std::visit([&](const auto& message) {
    supported(message,kMaxVoteWireBytes);
    if (!message.has_context()) throw std::invalid_argument("missing Raft response context");
    Envelope result{request.to,request.from,VoteResponse{}};
    check_context(message.context(),result,request);
    if constexpr (std::is_same_v<std::decay_t<decltype(message)>,pb::RequestVoteResponse>) {
      result.message = VoteResponse{message.term(),message.vote_granted()};
    } else {
      if (message.conflict_term() != 0) throw std::invalid_argument("unsupported Raft conflict-term optimization");
      result.message = AppendResponse{message.term(),message.context().rpc_id(),message.success(),message.match_index(),message.conflict_index()};
    }
    validate_response(result,request); return result;
  },response);
}
Envelope WireCodec::parse_request(WireMethod method, std::string_view bytes) const {
  if (method == WireMethod::vote) return decode_request(parse<pb::RequestVoteRequest>(bytes,kMaxVoteWireBytes));
  if (method == WireMethod::append) return decode_request(parse<pb::AppendEntriesRequest>(bytes,kMaxAppendWireBytes));
  throw std::invalid_argument("unsupported Raft method");
}
Envelope WireCodec::parse_response(std::string_view bytes, const Envelope& request) const {
  validate_request(request);
  if (std::holds_alternative<VoteRequest>(request.message)) return decode_response(parse<pb::RequestVoteResponse>(bytes,kMaxVoteWireBytes),request);
  return decode_response(parse<pb::AppendEntriesResponse>(bytes,kMaxVoteWireBytes),request);
}
std::string serialize_raft_wire(const WireRequest& request) { return serialize(request); }
std::string serialize_raft_wire(const WireResponse& response) { return serialize(response); }
} // namespace rollupx::raft
