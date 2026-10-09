#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rollupx {
struct Endpoint { std::string host; std::uint16_t port; };
struct Peer { std::string id; std::string endpoint; };
struct Config {
  std::string id;
  std::string cluster_id = "rollupx-local";
  std::string public_listen = "127.0.0.1:50051";
  std::string internal_listen = "127.0.0.1:50052";
  std::string http_listen = "127.0.0.1:9100";
  std::string data_dir;
  std::vector<Peer> peers; // Includes this node, stable on every member.
  std::chrono::milliseconds probe_interval{200};
  std::chrono::milliseconds rpc_timeout{150};
  std::chrono::milliseconds peer_stale_after{1500};
  void validate() const;
  [[nodiscard]] std::string topology() const;
  [[nodiscard]] std::string identity() const;
};
Endpoint parse_endpoint(const std::string& value);
Config parse_config(int argc, char* argv[]);
std::string usage();
} // namespace rollupx
