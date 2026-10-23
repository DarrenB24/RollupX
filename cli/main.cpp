#include "ledger.grpc.pb.h"
#include "rollupx/transaction_wire.hpp"
#include <charconv>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>

namespace {
using namespace rollupx;
void usage() {
  std::cerr << "rollupx-cli status|ready|leader HOST:PUBLIC_PORT\n"
    "rollupx-cli submit HOST:PUBLIC_PORT SIGNED_TRANSACTION_HEX\n"
    "rollupx-cli receipt HOST:PUBLIC_PORT TRANSACTION_ID_HEX\n"
    "rollupx-cli balance|nonce HOST:PUBLIC_PORT ADDRESS_HEX\n"
    "rollupx-cli batch HOST:PUBLIC_PORT LOG_INDEX\n"
    "Queries read local durable state; follower reads may lag. Submit to the discovered leader.\n";
}
void print(const v1::Receipt& receipt) {
  std::cout << "id=" << hex({reinterpret_cast<const Byte*>(receipt.transaction_id().data()),receipt.transaction_id().size()})
    << " state=" << v1::Receipt::State_Name(receipt.state()) << " log_index=" << receipt.log_index()
    << " batch=" << receipt.batch_number() << " rejection=" << receipt.rejection_reason() << '\n';
}
int checked(const grpc::Status& result) {
  if (result.ok()) return 0;
  std::cerr << "RPC " << result.error_code() << ": " << result.error_message() << '\n'; return 1;
}
} // namespace
int main(int argc, char* argv[]) {
  try {
    if (argc < 3) { usage(); return 2; }
    const std::string command(argv[1]);
    const bool simple = command == "status" || command == "ready" || command == "leader";
    const bool argument = command == "submit" || command == "receipt" || command == "balance" || command == "nonce" || command == "batch";
    if ((!simple && !argument) || argc != (simple ? 3 : 4)) { usage(); return 2; }
    auto stub = v1::Ledger::NewStub(grpc::CreateChannel(argv[2],grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(3));
    if (command == "status" || command == "ready") {
      v1::ClusterStatus response;
      if (checked(stub->GetClusterStatus(&context,v1::Empty{},&response))) return 1;
      std::cout << "node=" << response.node_id() << " cluster=" << response.cluster_id()
        << " stage=" << v1::ClusterStatus::Stage_Name(response.stage())
        << " transport_ready=" << (response.transport_ready() ? "true" : "false")
        << " role=" << v1::ClusterStatus::Role_Name(response.role()) << " term=" << response.term()
        << " leader=" << response.leader_id() << " commit_index=" << response.commit_index()
        << " applied_index=" << response.applied_index() << " ledger_ready=" << (response.ledger_ready() ? "true" : "false") << '\n';
      for (const auto& peer : response.peers()) {
        std::cout << "peer=" << peer.node_id() << " endpoint=" << peer.endpoint()
          << " reachable=" << (peer.reachable() ? "true" : "false")
          << " successful_probes=" << peer.successful_probes() << " failed_probes=" << peer.failed_probes() << '\n';
      }
      const bool ready = response.stage() == v1::ClusterStatus::REPLICATED_LEDGER ? response.ledger_ready() : response.transport_ready();
      return command == "ready" && !ready ? 1 : 0;
    }
    if (command == "leader") {
      v1::LeaderResponse response;
      if (checked(stub->GetLeader(&context,v1::Empty{},&response))) return 1;
      std::cout << "node=" << response.node_id() << " endpoint=" << response.endpoint() << " term=" << response.term() << '\n';
      return 0;
    }
    if (command == "submit" || command == "receipt") {
      v1::Receipt response;
      if (command == "submit") {
        const auto transaction = decode_transaction(fixed_hex<kTransactionBytes>(argv[3]));
        if (checked(stub->SubmitTransaction(&context,transaction_to_proto(transaction),&response))) return 1;
      } else {
        const auto id = fixed_hex<32>(argv[3]); v1::TransactionId request; request.set_id(id.data(),id.size());
        if (checked(stub->GetTransaction(&context,request,&response))) return 1;
      }
      print(response); return 0;
    }
    if (command == "balance" || command == "nonce") {
      const auto address = fixed_hex<20>(argv[3]); v1::AccountAddress request;
      request.set_address(address.data(),address.size());
      if (command == "balance") {
        v1::BalanceResponse response;
        if (checked(stub->GetBalance(&context,request,&response))) return 1;
        std::cout << "balance=" << response.balance() << " applied_index=" << response.applied_index() << '\n';
      } else {
        v1::NonceResponse response;
        if (checked(stub->GetAccountNonce(&context,request,&response))) return 1;
        std::cout << "nonce=" << response.nonce() << " applied_index=" << response.applied_index() << '\n';
      }
      return 0;
    }
    const std::string_view input(argv[3]); std::uint64_t index = 0;
    const auto [end,error] = std::from_chars(input.data(),input.data()+input.size(),index);
    if (error != std::errc{} || end != input.data()+input.size() || index == 0) throw std::invalid_argument("invalid batch index");
    v1::BatchRequest request; request.set_number(index); v1::Batch response;
    if (checked(stub->GetBatch(&context,request,&response))) return 1;
    std::cout << "batch=" << response.number() << " log_index=" << response.log_index() << " transactions=" << response.transactions_size() << '\n';
    for (const auto& transaction : response.transactions()) std::cout << hex(encode_transaction(transaction_from_proto(transaction))) << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
