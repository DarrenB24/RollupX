#include "../test_support.hpp"
#include "../network_support.hpp"
#include "../ledger_fixture.hpp"
#include "rollupx/node.hpp"
#include "rollupx/transaction_wire.hpp"
#include "ledger.grpc.pb.h"
#include <fstream>
#include <future>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <thread>

namespace {
using namespace rollupx;
using namespace std::chrono_literals;
template<class Predicate> bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now()+15s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(30ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
class ReplicatedCluster : public ::testing::Test {
 protected:
  void SetUp() override {
    test::Ports ports;
    const auto genesis = test::ledger_genesis();
    genesis_path = directory.path+"/genesis.txt";
    std::ofstream file(genesis_path);
    file << "chain 31337\naccount " << hex(genesis.accounts[0].address) << " 100\naccount "
      << hex(genesis.accounts[1].address) << " 0\n";
    file.close();
    for (int i = 0; i < 5; ++i) {
      Config config; config.id = "n"+std::to_string(i+1); config.data_dir = directory.path+"/"+config.id;
      config.genesis_file = genesis_path;
      config.public_listen = ports.next(); config.internal_listen = ports.next(); config.http_listen = ports.next();
      configs.push_back(config);
    }
    for (auto& config : configs) for (const auto& peer : configs) {
      config.peers.push_back({peer.id,peer.internal_listen});
      config.public_peers.push_back({peer.id,peer.public_listen});
    }
    ports.release();
    for (const auto& config : configs) {
      nodes.push_back(std::make_unique<Node>(config));
      clients.push_back(v1::Ledger::NewStub(grpc::CreateChannel(config.public_listen,grpc::InsecureChannelCredentials())));
    }
    ASSERT_TRUE(eventually([&] { return leader() >= 0; }));
  }
  int leader() {
    int result = -1;
    for (std::size_t i = 0; i < nodes.size(); ++i) if (nodes[i]) {
      grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+300ms);
      v1::ClusterStatus status;
      if (clients[i]->GetClusterStatus(&context,v1::Empty{},&status).ok() && status.ledger_ready()) {
        if (result != -1) return -1;
        result = static_cast<int>(i);
      }
    }
    return result;
  }
  grpc::Status submit(std::size_t i, const SignedTransaction& transaction, v1::Receipt& receipt) {
    grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+3s);
    return clients[i]->SubmitTransaction(&context,transaction_to_proto(transaction),&receipt);
  }
  bool receipt_at(std::size_t i, const SignedTransaction& transaction, const v1::Receipt& expected) {
    v1::TransactionId request; request.set_id(transaction.id.data(),transaction.id.size());
    grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+500ms);
    v1::Receipt receipt;
    return clients[i]->GetTransaction(&context,request,&receipt).ok() && receipt.SerializeAsString() == expected.SerializeAsString();
  }
  bool all_receipts(const SignedTransaction& transaction, const v1::Receipt& expected) {
    for (std::size_t i = 0; i < nodes.size(); ++i) if (nodes[i] && !receipt_at(i,transaction,expected)) return false;
    return true;
  }
  void check_accounts(std::uint64_t sender_balance, std::uint64_t nonce, std::uint64_t min_index) {
    const auto genesis = test::ledger_genesis();
    for (std::size_t i = 0; i < nodes.size(); ++i) if (nodes[i]) {
      for (std::size_t account = 0; account < 2; ++account) {
        v1::AccountAddress request; request.set_address(genesis.accounts[account].address.data(),20);
        grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+1s);
        v1::BalanceResponse balance;
        ASSERT_TRUE(clients[i]->GetBalance(&context,request,&balance).ok());
        EXPECT_EQ(balance.balance(),account == 0 ? sender_balance : 100-sender_balance);
        EXPECT_GE(balance.applied_index(),min_index);
        grpc::ClientContext nonce_context; nonce_context.set_deadline(std::chrono::system_clock::now()+1s);
        v1::NonceResponse result;
        ASSERT_TRUE(clients[i]->GetAccountNonce(&nonce_context,request,&result).ok());
        EXPECT_EQ(result.nonce(),account == 0 ? nonce : 0);
        EXPECT_GE(result.applied_index(),min_index);
      }
    }
  }
  test::Directory directory;
  std::string genesis_path;
  std::vector<Config> configs;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<v1::Ledger::Stub>> clients;
};
TEST_F(ReplicatedCluster, SignedTransferConcurrentRetriesQueriesAndFullRestart) {
  const int elected = leader(); ASSERT_GE(elected,0);
  const auto transaction = test::ledger_transaction();
  v1::Receipt confirmed;
  std::vector<std::future<std::pair<grpc::Status,v1::Receipt>>> initial;
  for (int i = 0; i < 6; ++i) initial.push_back(std::async(std::launch::async,[&] {
    v1::Receipt receipt; const auto result = submit(elected,transaction,receipt);
    return std::make_pair(result,receipt);
  }));
  for (auto& call : initial) {
    const auto [result,receipt] = call.get();
    if (result.ok()) {
      if (confirmed.state() != v1::Receipt::STATE_UNSPECIFIED)
        EXPECT_EQ(receipt.SerializeAsString(),confirmed.SerializeAsString());
      confirmed = receipt;
    } else EXPECT_EQ(result.error_code(),grpc::StatusCode::RESOURCE_EXHAUSTED);
  }
  ASSERT_EQ(confirmed.state(),v1::Receipt::APPLIED);
  EXPECT_GT(confirmed.log_index(),1U); // Election no-op is never a transfer batch.
  ASSERT_TRUE(eventually([&] { return all_receipts(transaction,confirmed); }));
  std::vector<std::future<bool>> retries;
  for (std::size_t i = 0; i < nodes.size(); ++i) retries.push_back(std::async(std::launch::async,[&,i] {
    v1::Receipt receipt; return submit(i,transaction,receipt).ok() && receipt.SerializeAsString() == confirmed.SerializeAsString();
  }));
  for (auto& retry : retries) EXPECT_TRUE(retry.get());
  check_accounts(90,1,confirmed.log_index());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    v1::BatchRequest request; request.set_number(confirmed.log_index());
    grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+1s);
    v1::Batch batch; ASSERT_TRUE(clients[i]->GetBatch(&context,request,&batch).ok());
    ASSERT_EQ(batch.transactions_size(),1); EXPECT_EQ(transaction_from_proto(batch.transactions(0)),transaction);
    EXPECT_EQ(batch.log_index(),confirmed.log_index()); EXPECT_TRUE(batch.transaction_root().empty());
    grpc::ClientContext leader_context; leader_context.set_deadline(std::chrono::system_clock::now()+1s);
    v1::LeaderResponse hint; ASSERT_TRUE(clients[i]->GetLeader(&leader_context,v1::Empty{},&hint).ok());
    EXPECT_EQ(hint.node_id(),configs[elected].id); EXPECT_EQ(hint.endpoint(),configs[elected].public_listen);
  }
  for (auto& node : nodes) node.reset();
  for (std::size_t i = 0; i < nodes.size(); ++i) nodes[i] = std::make_unique<Node>(configs[i]);
  ASSERT_TRUE(eventually([&] { return leader() >= 0 && all_receipts(transaction,confirmed); }));
  check_accounts(90,1,confirmed.log_index());
  v1::Receipt retry; ASSERT_TRUE(submit(leader(),transaction,retry).ok());
  EXPECT_EQ(retry.SerializeAsString(),confirmed.SerializeAsString());
}
TEST_F(ReplicatedCluster, MajoritySurvivesLeaderLossAndMinorityCannotConfirm) {
  int elected = leader(); ASSERT_GE(elected,0);
  const auto first = test::ledger_transaction(); v1::Receipt receipt;
  ASSERT_TRUE(submit(elected,first,receipt).ok());
  ASSERT_TRUE(eventually([&] { return all_receipts(first,receipt); }));
  const std::size_t lost_leader = static_cast<std::size_t>(elected);
  const std::size_t lost_follower = (lost_leader+1)%nodes.size();
  nodes[lost_leader].reset(); nodes[lost_follower].reset();
  ASSERT_TRUE(eventually([&] { elected = leader(); return elected >= 0; }));
  const auto second = test::ledger_transaction(1,20); v1::Receipt second_receipt;
  ASSERT_TRUE(submit(elected,second,second_receipt).ok());
  ASSERT_TRUE(eventually([&] { return all_receipts(second,second_receipt); }));
  check_accounts(70,2,second_receipt.log_index());
  std::size_t third = 0;
  while (!nodes[third] || third == static_cast<std::size_t>(elected)) ++third;
  nodes[third].reset();
  ASSERT_TRUE(eventually([&] { return leader() < 0; }));
  v1::Receipt unavailable;
  EXPECT_FALSE(submit(elected,test::ledger_transaction(2,5),unavailable).ok());
  EXPECT_EQ(unavailable.state(),v1::Receipt::STATE_UNSPECIFIED);
  check_accounts(70,2,second_receipt.log_index());
  for (std::size_t i = 0; i < nodes.size(); ++i) if (!nodes[i]) nodes[i] = std::make_unique<Node>(configs[i]);
  ASSERT_TRUE(eventually([&] { return leader() >= 0 && all_receipts(second,second_receipt); }));
  check_accounts(70,2,second_receipt.log_index());
}
TEST_F(ReplicatedCluster, InvalidAdmissionMissingQueriesAndUnsupportedProofsFailExplicitly) {
  const int elected = leader(); ASSERT_GE(elected,0);
  auto* stub = clients[elected].get();
  grpc::ClientContext malformed; malformed.set_deadline(std::chrono::system_clock::now()+1s);
  v1::Receipt receipt;
  EXPECT_EQ(stub->SubmitTransaction(&malformed,v1::Transaction{},&receipt).error_code(),grpc::StatusCode::INVALID_ARGUMENT);
  const auto overspend = test::ledger_transaction(0,101);
  EXPECT_EQ(submit(elected,overspend,receipt).error_code(),grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(receipt.state(),v1::Receipt::STATE_UNSPECIFIED);
  v1::TransactionId id; id.set_id(overspend.id.data(),overspend.id.size());
  grpc::ClientContext missing; missing.set_deadline(std::chrono::system_clock::now()+1s);
  EXPECT_EQ(stub->GetTransaction(&missing,id,&receipt).error_code(),grpc::StatusCode::NOT_FOUND);
  grpc::ClientContext proof_context; proof_context.set_deadline(std::chrono::system_clock::now()+1s);
  v1::MerkleProof proof;
  EXPECT_EQ(stub->GetTransactionProof(&proof_context,id,&proof).error_code(),grpc::StatusCode::UNIMPLEMENTED);
  v1::BatchRequest batch_request; batch_request.set_number(1); v1::Batch batch;
  grpc::ClientContext batch_context; batch_context.set_deadline(std::chrono::system_clock::now()+1s);
  EXPECT_EQ(stub->GetBatch(&batch_context,batch_request,&batch).error_code(),grpc::StatusCode::NOT_FOUND);
  v1::AccountAddress unknown; const Address empty{}; unknown.set_address(empty.data(),empty.size());
  grpc::ClientContext account_context; account_context.set_deadline(std::chrono::system_clock::now()+1s);
  v1::BalanceResponse balance;
  EXPECT_EQ(stub->GetBalance(&account_context,unknown,&balance).error_code(),grpc::StatusCode::NOT_FOUND);
  const std::size_t follower = (static_cast<std::size_t>(elected)+1)%nodes.size();
  EXPECT_EQ(submit(follower,test::ledger_transaction(),receipt).error_code(),grpc::StatusCode::UNAVAILABLE);
  check_accounts(100,0,0);
}
TEST_F(ReplicatedCluster, DurableIdentityRejectsChangedGenesisAndNetworkingModeReuse) {
  nodes[0].reset();
  auto network_config = configs[0]; network_config.genesis_file.clear(); network_config.public_peers.clear();
  EXPECT_THROW(Node node(network_config),std::runtime_error);
  std::ofstream file(genesis_path); const auto genesis = test::ledger_genesis();
  file << "chain 31338\naccount " << hex(genesis.accounts[0].address) << " 100\n"; file.close();
  EXPECT_THROW(Node node(configs[0]),std::runtime_error);
}
} // namespace
