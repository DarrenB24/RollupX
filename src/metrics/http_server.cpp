#include "rollupx/http_server.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace rollupx {
namespace {
using Clock = std::chrono::steady_clock;
class Fd {
 public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() { if (value_ >= 0) ::close(value_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return value_; }
  int release() { return std::exchange(value_, -1); }
 private:
  int value_;
};
void nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("HTTP fcntl failed");
#ifdef SO_NOSIGPIPE
  const int yes = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0)
    throw std::runtime_error("HTTP SO_NOSIGPIPE failed");
#endif
}
bool wait_fd(int fd, short events, Clock::time_point deadline, std::stop_token token) {
  while (!token.stop_requested()) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) return false;
    pollfd pfd{fd, events, 0};
    const int result = ::poll(&pfd, 1, static_cast<int>(std::min<std::int64_t>(remaining, 50)));
    if (result > 0) return (pfd.revents & events) != 0;
    if (result < 0 && errno != EINTR) return false;
  }
  return false;
}
std::string encode(const HttpResponse& response) {
  const char* reason = "Error";
  if (response.status == 200) reason = "OK";
  else if (response.status == 404) reason = "Not Found";
  else if (response.status == 400) reason = "Bad Request";
  else if (response.status == 503) reason = "Service Unavailable";
  return "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\nContent-Type: " +
    response.content_type + "\r\nContent-Length: " + std::to_string(response.body.size()) +
    "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + response.body;
}
void serve(int fd, const HttpServer::Handler& handler, std::stop_token token) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(250);
  std::string request;
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 4096) {
    if (!wait_fd(fd, POLLIN, deadline, token)) return;
    char buffer[512];
    const auto count = ::recv(fd, buffer, std::min(sizeof(buffer), 4096 - request.size()), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return;
    request.append(buffer, static_cast<std::size_t>(count));
  }
  HttpResponse response{400, "text/plain", "bad request\n"};
  const auto end = request.find("\r\n");
  if (request.find("\r\n\r\n") != std::string::npos && end != std::string::npos) {
    const auto line = request.substr(0, end);
    const auto space = line.find(' ', 4);
    if (line.starts_with("GET ") && space != std::string::npos &&
        (line.substr(space) == " HTTP/1.1" || line.substr(space) == " HTTP/1.0")) {
      try { response = handler(line.substr(4, space - 4)); }
      catch (...) { response = {500, "text/plain", "internal error\n"}; }
    }
  }
  const auto bytes = encode(response);
  std::size_t sent = 0;
  while (sent < bytes.size() && wait_fd(fd, POLLOUT, deadline, token)) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = ::send(fd, bytes.data() + sent, bytes.size() - sent, flags);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return;
    sent += static_cast<std::size_t>(count);
  }
}
} // namespace
HttpServer::HttpServer(Endpoint endpoint, Handler handler) : handler_(std::move(handler)) {
  if (!handler_) throw std::invalid_argument("HTTP handler is required");
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const auto service = std::to_string(endpoint.port);
  const int error = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &result);
  if (error != 0) throw std::runtime_error(std::string("HTTP getaddrinfo: ") + gai_strerror(error));
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(result, freeaddrinfo);
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd.get() < 0) throw std::runtime_error("HTTP socket failed");
  nonblocking(fd.get());
  const int yes = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
      ::bind(fd.get(), addresses->ai_addr, addresses->ai_addrlen) != 0 || ::listen(fd.get(), 32) != 0)
    throw std::runtime_error(std::string("HTTP bind/listen: ") + std::strerror(errno));
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &length) != 0)
    throw std::runtime_error("HTTP getsockname failed");
  port_ = ntohs(bound.sin_port);
  listener_ = fd.release();
  try { worker_ = std::jthread([this](std::stop_token token) { run(token); }); }
  catch (...) { ::close(listener_); listener_ = -1; throw; }
}
HttpServer::~HttpServer() { stop(); }
void HttpServer::stop() {
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
  if (listener_ >= 0) { ::close(listener_); listener_ = -1; }
}
void HttpServer::run(std::stop_token token) {
  while (!token.stop_requested()) {
    pollfd pfd{listener_, POLLIN, 0};
    const int result = ::poll(&pfd, 1, 50);
    if (result < 0 && errno == EINTR) continue;
    if (result < 0 || (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
      failed_.store(true); return;
    }
    if (result == 0) continue;
    Fd client(::accept(listener_, nullptr, nullptr));
    if (client.get() < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      failed_.store(true); return;
    }
    try { nonblocking(client.get()); serve(client.get(), handler_, token); }
    catch (...) { failed_.store(true); return; }
  }
}
} // namespace rollupx
