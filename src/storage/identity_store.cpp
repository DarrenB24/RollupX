#include "rollupx/rocksdb_open.hpp"
#include "rollupx/identity_store.hpp"
#include <filesystem>
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>
#include <stdexcept>

namespace rollupx {
IdentityStore::IdentityStore(const Config& config) {
  config.validate();
  std::filesystem::create_directories(config.data_dir);
  rocksdb::Options options;
  options.create_if_missing = true;
  options.paranoid_checks = true;
  options.max_open_files = 128;
  options.write_buffer_size = 4 * 1024 * 1024;
  options.max_write_buffer_number = 2;
  const auto status = open_rocksdb(options, config.data_dir, db_);
  if (!status.ok()) throw std::runtime_error("RocksDB open: " + status.ToString());
  std::string identity;
  const auto read = db_->Get(rocksdb::ReadOptions{}, "meta/identity", &identity);
  if (read.IsNotFound()) {
    // Never silently initialize an existing nonempty DB whose manifest disappeared.
    const std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(rocksdb::ReadOptions{}));
    iterator->SeekToFirst();
    if (iterator->Valid()) throw std::runtime_error("nonempty database lacks identity manifest");
    if (!iterator->status().ok()) throw std::runtime_error("RocksDB scan: " + iterator->status().ToString());
    rocksdb::WriteBatch batch;
    batch.Put("meta/identity", config.identity());
    rocksdb::WriteOptions write;
    write.sync = true;
    const auto saved = db_->Write(write, &batch);
    if (!saved.ok()) throw std::runtime_error("RocksDB identity write: " + saved.ToString());
  } else if (!read.ok()) throw std::runtime_error("RocksDB identity read: " + read.ToString());
  else if (identity != config.identity()) throw std::runtime_error("database format/node/cluster/topology mismatch");
}
IdentityStore::~IdentityStore() = default;
} // namespace rollupx
