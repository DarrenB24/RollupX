#include "rollupx/durable_ledger.hpp"
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>

namespace rollupx {
namespace {
constexpr auto kIdentity = "rollupx-standalone-ledger-v1";
void checked(const rocksdb::Status& status, const char* operation) {
  if (!status.ok()) throw std::runtime_error(std::string(operation) + ": " + status.ToString());
}
} // namespace
struct DurableLedger::Impl {
  Impl(const std::string& directory, Genesis initial, LedgerFailureHook failure_hook, LedgerOpenMode mode)
    : genesis(std::move(initial)), hook(std::move(failure_hook)) {
    const auto manifest = encode_genesis(genesis); // Validate before touching the filesystem.
    if (directory.empty()) throw std::invalid_argument("ledger directory is required");
    if (mode == LedgerOpenMode::create_or_open) std::filesystem::create_directories(directory);
    rocksdb::Options options;
    options.create_if_missing = mode == LedgerOpenMode::create_or_open; options.paranoid_checks = true;
    options.max_open_files = 128; options.write_buffer_size = 4*1024*1024; options.max_write_buffer_number = 2;
    rocksdb::DB* output = nullptr;
    const auto opened = rocksdb::DB::Open(options, directory, &output); db.reset(output); checked(opened, "open ledger");
    const auto identity = get("meta/identity");
    if (!identity) {
      if (mode == LedgerOpenMode::existing_only) throw std::runtime_error("database is not an initialized standalone ledger");
      const std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(rocksdb::ReadOptions{}));
      iterator->SeekToFirst();
      if (iterator->Valid()) throw std::runtime_error("nonempty ledger database lacks identity");
      checked(iterator->status(), "inspect empty ledger");
      state = {0, genesis.accounts};
      rocksdb::WriteBatch write;
      write.Put("meta/identity", kIdentity); write.Put("ledger/genesis", manifest);
      write.Put("ledger/state", encode_state(state)); write.Put("ledger/commit", encode_progress(0));
      sync(write);
    } else {
      if (*identity != kIdentity || require("ledger/genesis") != manifest)
        throw std::runtime_error("ledger identity, chain, or genesis mismatch");
      state = decode_state(require("ledger/state")); commit_index = decode_progress(require("ledger/commit"));
      validate_recovery_state(genesis, state, commit_index);
      // No compaction yet. Every committed command must still be available. Check
      // continuity before applying any pending entries; never repair missing history.
      const std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(rocksdb::ReadOptions{}));
      RecoveryAudit audit(genesis, state, commit_index,
        [this](const Hash& id) { return find_receipt(id); },
        [this](std::uint64_t index) { return load_batch(index); });
      for (iterator->Seek("log/"); iterator->Valid() && iterator->key().starts_with("log/"); iterator->Next()) {
        const auto index = decode_log_key(iterator->key().ToString());
        const auto command = decode_batch(iterator->value().ToString());
        audit.observe(index, command);
      }
      checked(iterator->status(), "scan ledger history");
      audit.finish();
      while (state.applied_index < commit_index) {
        const auto command = load_batch(state.applied_index+1);
        (void)apply(command); // Replays only the durable committed suffix.
      }
    }
  }
  std::optional<std::string> get(const std::string& key) const {
    std::string value; const auto status = db->Get(rocksdb::ReadOptions{}, key, &value);
    if (status.IsNotFound()) return std::nullopt;
    checked(status, "read ledger"); return value;
  }
  std::string require(const std::string& key) const {
    const auto value = get(key);
    if (!value) {
      throw std::runtime_error("missing required ledger key: " + key);
    }
    return *value;
  }
  void sync(rocksdb::WriteBatch& write) {
    rocksdb::WriteOptions options; options.sync = true; options.disableWAL = false;
    checked(db->Write(options, &write), "synchronous ledger write");
  }
  void ensure_healthy() const {
    if (poisoned) throw std::runtime_error("ledger failed; close and reopen to recover before reuse");
  }
  void at(LedgerCrashPoint point) { if (hook) hook(point); }
  std::optional<LedgerReceipt> find_receipt(const Hash& id) const {
    const auto encoded = get(receipt_key(id)); if (!encoded) return std::nullopt;
    const auto receipt = decode_receipt(*encoded);
    if (receipt.transaction_id != id || receipt.log_index > state.applied_index)
      throw std::runtime_error("receipt identity or applied progress mismatch");
    const auto original = load_batch(receipt.log_index);
    if (receipt.transaction_offset >= original.transactions.size() ||
        original.transactions[receipt.transaction_offset].id != id)
      throw std::runtime_error("receipt does not identify its original command");
    return receipt;
  }
  LedgerBatch load_batch(std::uint64_t index) const {
    if (cached_batch && cached_batch->index == index) return *cached_batch;
    auto command = decode_batch(require(log_key(index)));
    if (command.index != index) throw std::runtime_error("log key/index mismatch");
    cached_batch = command; // One bounded immutable batch; accessed under the ledger mutex.
    return command;
  }
  std::vector<LedgerReceipt> apply(const LedgerBatch& command) {
    if (state.applied_index == std::numeric_limits<std::uint64_t>::max() ||
        command.index != state.applied_index+1 || command.index > commit_index)
      throw std::runtime_error("attempt to apply an uncommitted or out-of-order batch");
    const auto plan = plan_application(genesis, state, commit_index, command,
      [this](const Hash& id) { return find_receipt(id); });
    rocksdb::WriteBatch write;
    for (const auto& receipt : plan.new_receipts) write.Put(receipt_key(receipt.transaction_id), encode_receipt(receipt));
    write.Put("ledger/state", encode_state(plan.state));
    at(LedgerCrashPoint::before_apply);
    sync(write); // Accounts, nonces, receipts and applied index share one WAL WriteBatch.
    state = plan.state; // Publish cached state only after the synchronous write completes.
    at(LedgerCrashPoint::after_apply);
    return plan.results;
  }
  std::vector<LedgerReceipt> commit(const std::vector<SignedTransaction>& transactions) {
    ensure_healthy();
    if (commit_index == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("ledger index exhausted");
    const LedgerBatch command{commit_index+1, transactions};
    const auto encoded = encode_batch(command); // Admission errors must not poison the ledger.
    try {
      at(LedgerCrashPoint::before_commit);
      rocksdb::WriteBatch write;
      write.Put(log_key(command.index), encoded); write.Put("ledger/commit", encode_progress(command.index));
      sync(write); commit_index = command.index;
      at(LedgerCrashPoint::after_commit);
      return apply(command);
    } catch (...) { poisoned = true; throw; } // Outcome can be durable despite a failed call.
  }
  const Genesis genesis;
  LedgerFailureHook hook;
  std::unique_ptr<rocksdb::DB> db;
  mutable std::mutex mutex;
  LedgerState state{};
  std::uint64_t commit_index = 0;
  bool poisoned = false;
  mutable std::optional<LedgerBatch> cached_batch;
};
DurableLedger::DurableLedger(std::string directory, Genesis genesis, LedgerFailureHook hook, LedgerOpenMode mode)
  : impl_(std::make_unique<Impl>(directory, std::move(genesis), std::move(hook), mode)) {}
DurableLedger::~DurableLedger() = default;
LedgerReceipt DurableLedger::submit(const SignedTransaction& transaction) {
  (void)authenticate(transaction);
  std::lock_guard lock(impl_->mutex); impl_->ensure_healthy();
  try {
    if (const auto prior = impl_->find_receipt(transaction.id)) return *prior;
  } catch (...) { impl_->poisoned = true; throw; }
  return impl_->commit({transaction}).front();
}
std::vector<LedgerReceipt> DurableLedger::commit_development_batch(const std::vector<SignedTransaction>& transactions) {
  if (transactions.empty() || transactions.size() > kMaxLedgerBatch) throw std::invalid_argument("batch count out of bounds");
  std::lock_guard lock(impl_->mutex); return impl_->commit(transactions);
}
LedgerState DurableLedger::snapshot() const {
  std::lock_guard lock(impl_->mutex); impl_->ensure_healthy(); return impl_->state;
}
std::optional<LedgerReceipt> DurableLedger::receipt(const Hash& id) const {
  std::lock_guard lock(impl_->mutex); impl_->ensure_healthy();
  try { return impl_->find_receipt(id); } catch (...) { impl_->poisoned = true; throw; }
}
std::optional<LedgerBatch> DurableLedger::batch(std::uint64_t index) const {
  std::lock_guard lock(impl_->mutex); impl_->ensure_healthy();
  if (index == 0 || index > impl_->commit_index) return std::nullopt;
  try { return impl_->load_batch(index); } catch (...) { impl_->poisoned = true; throw; }
}
} // namespace rollupx
