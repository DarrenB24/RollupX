#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace rollupx::test {
// Reserve distinct kernel-selected ports until the complete topology is assembled.
class Ports {
 public:
  ~Ports() { release(); }
  std::string next() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("test socket failed");
    descriptors.push_back(fd);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      throw std::runtime_error("test port reservation failed");
    socklen_t size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0)
      throw std::runtime_error("test getsockname failed");
    return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  }
  void release() { for (const int fd : descriptors) ::close(fd); descriptors.clear(); }
 private:
  std::vector<int> descriptors;
};
} // namespace rollupx::test
