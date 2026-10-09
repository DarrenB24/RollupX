#include "rollupx/config.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace rollupx {
namespace {
bool identifier(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
    std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
unsigned number(std::string_view value, unsigned maximum) {
  unsigned out = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size() || out == 0 || out > maximum)
    throw std::invalid_argument("invalid positive integer: " + std::string(value));
  return out;
}
} // namespace
Endpoint parse_endpoint(const std::string& value) {
  const auto colon = value.find(':');
  if (colon == std::string::npos || colon == 0 || value.find(':', colon + 1) != std::string::npos)
    throw std::invalid_argument("expected IPv4/hostname:port: " + value);
  const auto host = value.substr(0, colon);
  if (host.size() > 253 || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '.' || c == '-';
  })) throw std::invalid_argument("invalid endpoint hostname: " + host);
  return {host, static_cast<std::uint16_t>(number(std::string_view(value).substr(colon + 1), 65535))};
}
void Config::validate() const {
  if (!identifier(id) || !identifier(cluster_id)) throw std::invalid_argument("invalid node/cluster ID");
  if (data_dir.empty()) throw std::invalid_argument("--data-dir is required");
  for (const auto& endpoint : {public_listen, internal_listen, http_listen}) (void)parse_endpoint(endpoint);
  // Conservatively reject shared listener ports even when host strings differ.
  const std::set<std::uint16_t> ports{parse_endpoint(public_listen).port,
    parse_endpoint(internal_listen).port, parse_endpoint(http_listen).port};
  if (ports.size() != 3) throw std::invalid_argument("listeners require distinct ports");
  if (peers.empty() || peers.size() > 31 || peers.size() % 2 == 0)
    throw std::invalid_argument("membership must contain 1..31 odd members (normally 5)");
  std::set<std::string> ids, endpoints;
  for (const auto& peer : peers) {
    if (!identifier(peer.id)) throw std::invalid_argument("invalid member ID");
    const auto endpoint = parse_endpoint(peer.endpoint);
    if (endpoint.host == "0.0.0.0") throw std::invalid_argument("member endpoints must be routable");
    if (!ids.insert(peer.id).second || !endpoints.insert(peer.endpoint).second)
      throw std::invalid_argument("duplicate member ID or endpoint");
  }
  if (!ids.contains(id)) throw std::invalid_argument("node ID must appear in membership");
  if (probe_interval.count() < 10 || probe_interval.count() > 60000 ||
      rpc_timeout.count() < 10 || rpc_timeout.count() > 10000 ||
      peer_stale_after < 2 * (probe_interval + rpc_timeout) || peer_stale_after.count() > 300000)
    throw std::invalid_argument("invalid probe timing bounds");
}
std::string Config::topology() const {
  auto sorted = peers;
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  std::string result;
  for (const auto& peer : sorted) result += peer.id + "=" + peer.endpoint + "\n";
  return result;
}
std::string Config::identity() const { return "rollupx-format-1\n" + cluster_id + "\n" + id + "\n" + topology(); }
Config parse_config(int argc, char* argv[]) {
  Config config;
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(argv[i]));
    if (!options.emplace(argv[i], argv[i + 1]).second) throw std::invalid_argument("duplicate option");
  }
  for (const auto& [key, value] : options) {
    if (key == "--id") config.id = value;
    else if (key == "--cluster-id") config.cluster_id = value;
    else if (key == "--public-listen") config.public_listen = value;
    else if (key == "--internal-listen") config.internal_listen = value;
    else if (key == "--http-listen") config.http_listen = value;
    else if (key == "--data-dir") config.data_dir = value;
    else if (key == "--probe-interval-ms") config.probe_interval = std::chrono::milliseconds(number(value, 60000));
    else if (key == "--rpc-timeout-ms") config.rpc_timeout = std::chrono::milliseconds(number(value, 10000));
    else if (key == "--peer-stale-ms") config.peer_stale_after = std::chrono::milliseconds(number(value, 300000));
    else if (key == "--peers") {
      std::size_t start = 0;
      while (start < value.size()) {
        const auto comma = value.find(',', start);
        const auto item = value.substr(start, comma == std::string::npos ? comma : comma - start);
        const auto equal = item.find('=');
        if (equal == std::string::npos) throw std::invalid_argument("members use id=host:port");
        config.peers.push_back({item.substr(0, equal), item.substr(equal + 1)});
        if (comma == std::string::npos) break;
        start = comma + 1;
        if (start == value.size()) throw std::invalid_argument("trailing membership separator");
      }
    } else throw std::invalid_argument("unknown option: " + key);
  }
  config.validate();
  return config;
}
std::string usage() {
  return "rollupx-node --id n1 --data-dir ./data/n1 --peers n1=127.0.0.1:50052,...\n"
    "  [--cluster-id rollupx-local] [--public-listen 127.0.0.1:50051]\n"
    "  [--internal-listen 127.0.0.1:50052] [--http-listen 127.0.0.1:9100]\n"
    "  [--probe-interval-ms 200] [--rpc-timeout-ms 150] [--peer-stale-ms 1500]\n";
}
} // namespace rollupx
