#include "rollupx/config.hpp"
#include "rollupx/http_server.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F function) {
  bool rejected = false;
  try { function(); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid configuration was accepted");
}
rollupx::Config sample() {
  rollupx::Config c;
  c.id = "n1"; c.data_dir = "/tmp/rollupx-test";
  c.peers = {{"n1", "127.0.0.1:50052"}, {"n2", "127.0.0.1:50062"}, {"n3", "127.0.0.1:50072"}};
  return c;
}
class Client {
 public:
  explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
    require(fd_ >= 0, "client socket failed");
    timeval timeout{2, 0};
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd_); throw std::runtime_error("HTTP connect failed");
    }
  }
  ~Client() { ::close(fd_); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  std::string request(const std::string& bytes) {
    require(::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()), "HTTP send failed");
    std::string response;
    char buffer[1024];
    while (true) {
      const auto count = ::recv(fd_, buffer, sizeof(buffer), 0);
      require(count >= 0, "HTTP receive timed out");
      if (count == 0) break;
      response.append(buffer, static_cast<std::size_t>(count));
    }
    return response;
  }
 private:
  int fd_;
};
void config_checks() {
  auto config = sample(); config.validate();
  auto reordered = config; std::swap(reordered.peers[0], reordered.peers[2]);
  require(config.identity() == reordered.identity(), "topology depends on argument order");
  for (const std::string bad : {"", ":90", "localhost:0", "localhost:65536", "localhost:-1", "x:1junk", "x:1:2", "x/y:90"})
    rejects([&] { (void)rollupx::parse_endpoint(bad); });
  auto bad = config; bad.peers[1].id = "n1"; rejects([&] { bad.validate(); });
  bad = config; bad.peers.pop_back(); rejects([&] { bad.validate(); });
  bad = config; bad.id = "missing"; rejects([&] { bad.validate(); });
  bad = config; bad.id = "label\"inject"; rejects([&] { bad.validate(); });
  bad = config; bad.peers[1].endpoint = bad.peers[0].endpoint; rejects([&] { bad.validate(); });
  bad = config; bad.http_listen = "0.0.0.0:50051"; rejects([&] { bad.validate(); });
  bad = config; bad.peer_stale_after = std::chrono::milliseconds(100); rejects([&] { bad.validate(); });
  char executable[] = "node", option[] = "--wat", value[] = "value";
  char* args[] = {executable, option, value};
  rejects([&] { (void)rollupx::parse_config(3, args); });
  rejects([&] { (void)rollupx::parse_config(2, args); });
  std::cout << "PASS configuration validation and canonical identity\n";
}
void http_checks() {
  rollupx::HttpServer server({"127.0.0.1", 0}, [](const std::string& path) -> rollupx::HttpResponse {
    if (path == "/healthz") return {200, "text/plain", "ok\n"};
    if (path == "/readyz") return {503, "text/plain", "not ready\n"};
    if (path == "/throw") throw std::runtime_error("test");
    return {404, "text/plain", "not found\n"};
  });
  require(Client(server.port()).request("GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n").ends_with("ok\n"), "health body missing");
  require(Client(server.port()).request("GET /readyz HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 503"), "readiness code wrong");
  require(Client(server.port()).request("GET /missing HTTP/1.0\r\n\r\n").starts_with("HTTP/1.1 404"), "unknown path not rejected");
  require(Client(server.port()).request("POST /healthz HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 400"), "bad method accepted");
  require(Client(server.port()).request("GET /throw HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 500"), "callback error hidden");
  const auto begin = std::chrono::steady_clock::now();
  require(Client(server.port()).request("GET /healthz").empty(), "incomplete request accepted");
  require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "slow request blocks worker");
  require(server.healthy(), "HTTP worker failed");
  Client idle(server.port());
  const auto shutdown = std::chrono::steady_clock::now();
  server.stop(); server.stop();
  require(std::chrono::steady_clock::now() - shutdown < std::chrono::seconds(1), "shutdown unbounded");
  std::cout << "PASS HTTP health, readiness, malformed requests, deadlines and shutdown\n";
}
} // namespace
int main(int argc, char* argv[]) {
  try {
    config_checks();
    if (argc == 2 && std::string(argv[1]) == "--config-only") return 0;
    http_checks(); return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
