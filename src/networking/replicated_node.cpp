#include "rollupx/replicated_node.hpp"
#include "rollupx/ledger_submission.hpp"
#include "rollupx/raft_grpc.hpp"
#include "rollupx/raft_store.hpp"
#include "rollupx/transaction_wire.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>

namespace rollupx {
namespace {
Genesis read_genesis(const std::string& path) {
  if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("genesis must be a regular file");
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open genesis");
  std::array<char,4097> bytes{};
  file.read(bytes.data(),bytes.size());
  if (file.bad()) throw std::runtime_error("cannot read genesis");
  return parse_genesis_text({bytes.data(),static_cast<std::size_t>(file.gcount())});
}
raft::Manifest manifest(const Config& config, const Genesis& genesis) {
  config.validate();
  std::vector<std::string> names;
  for (const auto& peer : config.peers) names.push_back(peer.id);
  std::sort(names.begin(),names.end());
  const auto local = static_cast<raft::NodeId>(std::find(names.begin(),names.end(),config.id)-names.begin()+1);
  const auto encoded = encode_genesis(genesis);
  return raft::make_named_manifest(config.cluster_id,names,
    keccak256({reinterpret_cast<const Byte*>(encoded.data()),encoded.size()}),local);
}
std::map<raft::NodeId,std::string> endpoints(const Config& config, const raft::Manifest& identity) {
  std::map<raft::NodeId,std::string> result;
  for (const auto& peer : config.peers) if (peer.id != config.id) {
    const auto id = static_cast<raft::NodeId>(std::find(identity.member_names.begin(),identity.member_names.end(),peer.id)
      -identity.member_names.begin()+1);
    result.emplace(id,peer.endpoint);
  }
  return result;
}
void receipt_to_proto(const LedgerReceipt& receipt, v1::Receipt& out) {
  out.set_transaction_id(receipt.transaction_id.data(),receipt.transaction_id.size());
  out.set_state(receipt.result == TransferResult::applied ? v1::Receipt::APPLIED : v1::Receipt::REJECTED);
  out.set_log_index(receipt.log_index);
  out.set_batch_number(receipt.log_index);
  if (receipt.result != TransferResult::applied) out.set_rejection_reason(std::string(result_name(receipt.result)));
}
} // namespace
struct ReplicatedNode::Impl {
  Impl(Config value, std::function<v1::ClusterStatus()> callback)
    : config(std::move(value)), genesis(read_genesis(config.genesis_file)), identity(manifest(config,genesis)),
      store(config.data_dir,identity,genesis), client(identity,endpoints(config,identity)),
      dispatcher(raft::WireCodec(identity),[this](auto peer, const auto& request, auto deadline, auto token) {
        return client.call(peer,request,deadline,token);
      },config.rpc_timeout), cluster_status(std::move(callback)) {
    // Per-process entropy avoids synchronized election sequences after a fleet restart.
    std::random_device random;
    const auto seed = (static_cast<std::uint64_t>(random()) << 32) ^ random();
    runtime = std::make_unique<raft::Runtime>(raft::Config{identity.node_id,identity.members,50,600,1200,seed},
      store.load(),[this](const auto& state) { store.persist(state); },
      [this](auto) { return store.apply_committed(); },
      [this](const auto& message) { return dispatcher.try_enqueue(message); },
      raft::RuntimeOptions{},raft::RuntimeClock{},ledger_proposal_policy(genesis,
        [this] { return store.ledger_state(); },[this](const auto& id) { return store.receipt(id); }));
    dispatcher.bind(runtime->handle());
    ingress = std::make_unique<raft::RaftIngress>(raft::WireCodec(identity),runtime->handle());
    service = std::make_unique<raft::GrpcRaftService>(*ingress);
    submission = std::make_unique<LedgerSubmission>(runtime->handle(),[this](const auto& id) { return store.receipt(id); });
  }
  ~Impl() { stop(); }
  bool healthy() const {
    const auto state = runtime->status();
    return !closed.load() && !failed.load() && !state.failed && !state.stopped &&
      !dispatcher.status().failed && !submission->status().failed;
  }
  void request_stop() {
    closed.store(true);
    if (submission) submission->close();
    if (ingress) ingress->close();
    if (runtime) runtime->request_stop();
    dispatcher.request_stop();
  }
  void stop() {
    request_stop();
    if (runtime) runtime->stop();
    dispatcher.stop();
  }
  grpc::Status fail() {
    failed.store(true); request_stop();
    return {grpc::StatusCode::INTERNAL,"replicated ledger failed; node must restart"};
  }
  bool ready(const raft::RuntimeStatus& status) {
    if (!healthy() || status.role != raft::Role::leader || status.applied == 0 || status.applied != status.commit) return false;
    try {
      const auto state = store.load();
      // A committed entry in this term proves completion of the leader's no-op barrier.
      return state.term == status.term && state.commit >= status.commit &&
        state.log.at(status.commit-1).term == status.term;
    } catch (...) { (void)fail(); return false; }
  }
  template<class Decode, class Execute>
  grpc::Status serve(grpc::ServerContext* context, Decode decode, Execute execute) {
    if (!healthy()) return {grpc::StatusCode::UNAVAILABLE,"replicated ledger unavailable"};
    if (context->deadline() <= std::chrono::system_clock::now())
      return {grpc::StatusCode::DEADLINE_EXCEEDED,"request deadline expired"};
    if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED,"request cancelled"};
    // Input errors are distinct from corruption/IO errors thrown by store reads.
    std::optional<decltype(decode())> input;
    try { input = decode(); }
    catch (const std::invalid_argument& error) { return {grpc::StatusCode::INVALID_ARGUMENT,error.what()}; }
    catch (...) { return fail(); }
    try { return execute(*input); }
    catch (...) { return fail(); }
  }
  std::string name(raft::NodeId id) const { return identity.member_names.at(id-1); }
  std::optional<Account> account(const LedgerState& state, const Address& wanted) const {
    for (const auto& value : state.accounts) if (value.address == wanted && wanted != Address{}) return value;
    return {};
  }
  const Config config;
  const Genesis genesis;
  const raft::Manifest identity;
  raft::Store store;
  raft::GrpcRaftClient client;
  raft::PeerDispatcher dispatcher;
  std::unique_ptr<raft::Runtime> runtime;
  std::unique_ptr<raft::RaftIngress> ingress;
  std::unique_ptr<raft::GrpcRaftService> service;
  std::unique_ptr<LedgerSubmission> submission;
  std::function<v1::ClusterStatus()> cluster_status;
  std::atomic<bool> closed{false}, failed{false};
};
ReplicatedNode::ReplicatedNode(Config config, std::function<v1::ClusterStatus()> status)
  : impl_(std::make_unique<Impl>(std::move(config),std::move(status))) {}
ReplicatedNode::~ReplicatedNode() = default;
grpc::Service* ReplicatedNode::raft_service() { return impl_->service.get(); }
bool ReplicatedNode::healthy() const { return impl_->healthy(); }
void ReplicatedNode::request_stop() { impl_->request_stop(); }
void ReplicatedNode::stop() { impl_->stop(); }
bool ReplicatedNode::ready() const {
  return impl_->ready(impl_->runtime->status());
}
v1::ClusterStatus ReplicatedNode::status() const {
  v1::ClusterStatus out;
  out.set_node_id(impl_->config.id); out.set_cluster_id(impl_->config.cluster_id);
  out.set_stage(v1::ClusterStatus::REPLICATED_LEDGER);
  const auto state = impl_->runtime->status();
  out.set_role(state.role == raft::Role::leader ? v1::ClusterStatus::LEADER :
    state.role == raft::Role::candidate ? v1::ClusterStatus::CANDIDATE : v1::ClusterStatus::FOLLOWER);
  out.set_term(state.term); out.set_commit_index(state.commit); out.set_applied_index(state.applied);
  if (state.leader && healthy()) out.set_leader_id(impl_->name(*state.leader));
  out.set_ledger_ready(impl_->ready(state));
  return out;
}
grpc::Status ReplicatedNode::SubmitTransaction(grpc::ServerContext* context, const v1::Transaction* request, v1::Receipt* out) {
  return impl_->serve(context,[&] { return transaction_from_proto(*request); },[&](const auto& transaction) -> grpc::Status {
    const auto now = std::chrono::system_clock::now();
    const auto budget = std::min(context->deadline(),now+std::chrono::seconds(2))-now;
    const auto result = impl_->submission->submit(transaction,std::chrono::steady_clock::now()+budget,
      [&] { return context->IsCancelled(); });
    switch (result.code) {
      case SubmissionCode::confirmed:
        if (!result.receipt) return impl_->fail();
        receipt_to_proto(*result.receipt,*out); return grpc::Status::OK;
      case SubmissionCode::invalid: return {grpc::StatusCode::INVALID_ARGUMENT,"invalid transaction"};
      case SubmissionCode::rejected: return {grpc::StatusCode::FAILED_PRECONDITION,result.reason};
      case SubmissionCode::busy: return {grpc::StatusCode::RESOURCE_EXHAUSTED,"sequencer busy or log full"};
      case SubmissionCode::unavailable: return {grpc::StatusCode::UNAVAILABLE,"leader unavailable; retry by transaction ID"};
      case SubmissionCode::deadline: return {grpc::StatusCode::DEADLINE_EXCEEDED,"confirmation expired; transaction may still commit"};
      case SubmissionCode::cancelled: return {grpc::StatusCode::CANCELLED,"cancelled; transaction may still commit"};
      case SubmissionCode::failed: return impl_->fail();
    }
    return impl_->fail();
  });
}
grpc::Status ReplicatedNode::GetTransaction(grpc::ServerContext* context, const v1::TransactionId* request, v1::Receipt* out) {
  return impl_->serve(context,[&] { return transaction_id_from_proto(*request); },[&](const auto& id) -> grpc::Status {
    const auto receipt = impl_->store.receipt(id);
    if (!receipt) return {grpc::StatusCode::NOT_FOUND,"no locally applied receipt; transaction may still commit"};
    receipt_to_proto(*receipt,*out); return grpc::Status::OK;
  });
}
grpc::Status ReplicatedNode::GetBalance(grpc::ServerContext* context, const v1::AccountAddress* request, v1::BalanceResponse* out) {
  return impl_->serve(context,[&] { return account_address_from_proto(*request); },[&](const auto& address) -> grpc::Status {
    const auto state = impl_->store.ledger_state(); const auto account = impl_->account(state,address);
    if (!account) return {grpc::StatusCode::NOT_FOUND,"account not in genesis registry"};
    out->set_balance(account->balance); out->set_applied_index(state.applied_index); return grpc::Status::OK;
  });
}
grpc::Status ReplicatedNode::GetAccountNonce(grpc::ServerContext* context, const v1::AccountAddress* request, v1::NonceResponse* out) {
  return impl_->serve(context,[&] { return account_address_from_proto(*request); },[&](const auto& address) -> grpc::Status {
    const auto state = impl_->store.ledger_state(); const auto account = impl_->account(state,address);
    if (!account) return {grpc::StatusCode::NOT_FOUND,"account not in genesis registry"};
    out->set_nonce(account->nonce); out->set_applied_index(state.applied_index); return grpc::Status::OK;
  });
}
grpc::Status ReplicatedNode::GetBatch(grpc::ServerContext* context, const v1::BatchRequest* request, v1::Batch* out) {
  return impl_->serve(context,[&] {
    if (request->number() == 0 || !request->GetReflection()->GetUnknownFields(*request).empty())
      throw std::invalid_argument("batch number must be a positive Raft index; unknown fields unsupported");
    return request->number();
  },[&](auto index) -> grpc::Status {
    const auto batch = impl_->store.batch(index);
    if (!batch) return {grpc::StatusCode::NOT_FOUND,"no applied transfer batch at this log index"};
    out->set_number(index); out->set_log_index(index);
    for (const auto& transaction : batch->transactions) *out->add_transactions() = transaction_to_proto(transaction);
    // Merkle roots and settlement commitments remain absent until implemented.
    return grpc::Status::OK;
  });
}
grpc::Status ReplicatedNode::GetBlock(grpc::ServerContext* context, const v1::BatchRequest* request, v1::Batch* out) {
  return GetBatch(context,request,out);
}
grpc::Status ReplicatedNode::GetClusterStatus(grpc::ServerContext* context, const v1::Empty*, v1::ClusterStatus* out) {
  return impl_->serve(context,[] { return 0; },[&](auto) {
    *out = impl_->cluster_status(); return grpc::Status::OK;
  });
}
grpc::Status ReplicatedNode::GetLeader(grpc::ServerContext* context, const v1::Empty*, v1::LeaderResponse* out) {
  return impl_->serve(context,[] { return 0; },[&](auto) -> grpc::Status {
    const auto state = impl_->runtime->status();
    if (!state.leader) return {grpc::StatusCode::UNAVAILABLE,"leader not known"};
    const auto name = impl_->name(*state.leader);
    out->set_node_id(name); out->set_term(state.term);
    for (const auto& peer : impl_->config.public_peers) if (peer.id == name) out->set_endpoint(peer.endpoint);
    return grpc::Status::OK;
  });
}
} // namespace rollupx
