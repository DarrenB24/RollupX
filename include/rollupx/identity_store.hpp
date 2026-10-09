#pragma once
#include "rollupx/config.hpp"
#include <memory>
#include <rocksdb/db.h>

namespace rollupx {
// One exclusive RocksDB owner per directory. Phase 1 stores format/identity only.
class IdentityStore {
 public:
  explicit IdentityStore(const Config& config);
  ~IdentityStore();
  IdentityStore(const IdentityStore&) = delete;
  IdentityStore& operator=(const IdentityStore&) = delete;
 private:
  std::unique_ptr<rocksdb::DB> db_;
};
} // namespace rollupx
