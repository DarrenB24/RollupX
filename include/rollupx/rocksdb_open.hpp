#pragma once
#include <memory>
#include <rocksdb/db.h>

namespace rollupx {
// RocksDB releases expose either a raw output pointer or an owning output pointer.
// Keep ownership immediate with both the Ubuntu CI and current Homebrew APIs.
template<class Database = rocksdb::DB>
rocksdb::Status open_rocksdb(const rocksdb::Options& options, const std::string& path,
    std::unique_ptr<Database>& database) {
  if constexpr (requires { Database::Open(options,path,&database); }) {
    return Database::Open(options,path,&database);
  } else {
    Database* opened = nullptr;
    const auto status = Database::Open(options,path,&opened);
    database.reset(opened);
    return status;
  }
}
} // namespace rollupx
