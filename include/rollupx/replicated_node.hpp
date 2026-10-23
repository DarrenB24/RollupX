#pragma once
#include "rollupx/config.hpp"
#include "ledger.grpc.pb.h"
#include <functional>
#include <memory>

namespace rollupx {
// Owns one audited ledger/Raft store and its entire runtime/transport graph.
// The host must request_stop(), stop and wait for both gRPC servers, then stop()
// before destruction; no handler or callback may outlive this object.
class ReplicatedNode final : public v1::Ledger::Service {
 public:
  ReplicatedNode(Config config, std::function<v1::ClusterStatus()> cluster_status);
  ~ReplicatedNode();
  grpc::Service* raft_service();
  v1::ClusterStatus status() const;
  bool healthy() const;
  bool ready() const;
  void request_stop();
  void stop();
  grpc::Status SubmitTransaction(grpc::ServerContext*, const v1::Transaction*, v1::Receipt*) override;
  grpc::Status GetTransaction(grpc::ServerContext*, const v1::TransactionId*, v1::Receipt*) override;
  grpc::Status GetBalance(grpc::ServerContext*, const v1::AccountAddress*, v1::BalanceResponse*) override;
  grpc::Status GetAccountNonce(grpc::ServerContext*, const v1::AccountAddress*, v1::NonceResponse*) override;
  grpc::Status GetBatch(grpc::ServerContext*, const v1::BatchRequest*, v1::Batch*) override;
  grpc::Status GetBlock(grpc::ServerContext*, const v1::BatchRequest*, v1::Batch*) override;
  grpc::Status GetClusterStatus(grpc::ServerContext*, const v1::Empty*, v1::ClusterStatus*) override;
  grpc::Status GetLeader(grpc::ServerContext*, const v1::Empty*, v1::LeaderResponse*) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx
