#pragma once
#include "rollupx/config.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace rollupx {
struct HttpResponse { int status; std::string content_type; std::string body; };
// Single bounded HTTP worker; serves tiny operational endpoints, not ledger traffic.
// Callbacks must be short and thread-safe. No owning raw pointers.
class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const std::string&)>;
  HttpServer(Endpoint endpoint, Handler handler);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  void stop(); // Lifecycle operations are serialized by the owner.
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] bool healthy() const { return !failed_.load(); }
 private:
  void run(std::stop_token token);
  int listener_ = -1;
  std::uint16_t port_ = 0;
  Handler handler_;
  std::atomic<bool> failed_{false};
  std::jthread worker_;
};
} // namespace rollupx
