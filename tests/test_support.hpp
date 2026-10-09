#pragma once
#include "rollupx/config.hpp"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace rollupx::test {
class Directory {
 public:
  Directory() {
    auto pattern = (std::filesystem::temp_directory_path() / "rollupx-test-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
    const char* result = ::mkdtemp(buffer.data());
    if (!result) throw std::runtime_error("mkdtemp failed");
    path = result;
  }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  std::string path;
};
inline Config config(const std::string& directory) {
  Config value; value.id = "n1"; value.data_dir = directory;
  value.peers = {{"n1", "127.0.0.1:50052"}, {"n2", "127.0.0.1:50062"}, {"n3", "127.0.0.1:50072"}};
  return value;
}
} // namespace rollupx::test
