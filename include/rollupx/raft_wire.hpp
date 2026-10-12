#pragma once
#include "rollupx/raft_records.hpp"
#include "consensus.pb.h"

namespace rollupx::raft {
using WireRequest = std::variant<internal::v1::RequestVoteRequest, internal::v1::AppendEntriesRequest>;
using WireResponse = std::variant<internal::v1::RequestVoteResponse, internal::v1::AppendEntriesResponse>;
enum class WireMethod { vote, append };
inline constexpr std::size_t kMaxVoteWireBytes = 4096;
inline constexpr std::size_t kMaxAppendWireBytes = kMaxAppendBytes + 4096;
// Stateless codec: no sockets, persistence, or quorum decisions. Numeric IDs are
// positions+1 in the caller-supplied sorted member names. Never sort silently.
class WireCodec {
 public:
  WireCodec(std::string cluster, std::vector<std::string> members, Hash genesis, NodeId local);
  explicit WireCodec(Manifest identity);
  const Manifest& manifest() const { return identity_; }
  const Hash& configuration_id() const { return configuration_; }
  WireRequest encode_request(const Envelope& request) const;
  Envelope decode_request(const WireRequest& request) const;
  WireResponse encode_response(const Envelope& response, const Envelope& request) const;
  // request is the original outbound call, retained by the transport callback.
  Envelope decode_response(const WireResponse& response, const Envelope& request) const;
  Envelope parse_request(WireMethod method, std::string_view bytes) const;
  Envelope parse_response(std::string_view bytes, const Envelope& request) const;
 private:
  const std::string& name(NodeId id) const;
  NodeId id(std::string_view name) const;
  void route(const Envelope& message, bool outbound) const;
  internal::v1::RaftContext context(const Envelope& message, const Envelope& request) const;
  void check_context(const internal::v1::RaftContext& context, const Envelope& message,
    const Envelope& request) const;
  Manifest identity_;
  Hash configuration_;
};
std::string serialize_raft_wire(const WireRequest& request);
std::string serialize_raft_wire(const WireResponse& response);
} // namespace rollupx::raft
