#include "ledger.grpc.pb.h"
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc != 3 || (std::string(argv[1]) != "status" && std::string(argv[1]) != "ready")) {
    std::cerr << "usage: rollupx-cli status|ready HOST:PUBLIC_PORT\n"
              << "Phase 1 supports transport status only; signing and ledger commands are pending.\n";
    return 2;
  }
  auto stub = rollupx::v1::Ledger::NewStub(grpc::CreateChannel(argv[2], grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  rollupx::v1::ClusterStatus response;
  const auto result = stub->GetClusterStatus(&context, rollupx::v1::Empty{}, &response);
  if (!result.ok()) { std::cerr << "RPC " << result.error_code() << ": " << result.error_message() << '\n'; return 1; }
  std::cout << "node=" << response.node_id() << " cluster=" << response.cluster_id()
            << " stage=" << rollupx::v1::ClusterStatus::Stage_Name(response.stage())
            << " transport_ready=" << (response.transport_ready() ? "true" : "false") << '\n';
  for (const auto& peer : response.peers()) {
    std::cout << "peer=" << peer.node_id() << " endpoint=" << peer.endpoint()
              << " reachable=" << (peer.reachable() ? "true" : "false")
              << " successful_probes=" << peer.successful_probes() << " failed_probes=" << peer.failed_probes() << '\n';
  }
  return std::string(argv[1]) == "ready" && !response.transport_ready() ? 1 : 0;
}
