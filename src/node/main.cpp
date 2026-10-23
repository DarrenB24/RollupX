#include "rollupx/node.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
extern "C" void on_signal(int) { stopping = 1; }
}
int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << rollupx::usage(); return 0; }
    const auto config = rollupx::parse_config(argc, argv);
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0)
      throw std::runtime_error("cannot install shutdown signal handlers");
    rollupx::Node node(config);
    std::cout << "node=" << config.id << " stage=" << (config.genesis_file.empty() ? "NETWORKING_ONLY" : "REPLICATED_LEDGER")
              << " public=" << config.public_listen
              << " internal=" << config.internal_listen << " http=" << config.http_listen << std::endl;
    while (!stopping && node.healthy()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool failed = !stopping;
    node.stop();
    return failed ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n' << rollupx::usage(); return 1;
  }
}
