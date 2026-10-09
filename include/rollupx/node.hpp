#pragma once
#include "rollupx/config.hpp"
#include <memory>

namespace rollupx {
class Node {
 public:
  explicit Node(Config config);
  ~Node();
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  void stop();
  [[nodiscard]] bool healthy() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx
