#include "rollupx/rocksdb_open.hpp"
#include "rollupx/raft_store.hpp"
#include <filesystem>
#include <mutex>
#include <utility>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/write_batch.h>

namespace rollupx::raft {
namespace {
void checked(const rocksdb::Status& status, const char* operation) {
  if (!status.ok()) throw std::runtime_error(std::string(operation)+": "+status.ToString());
}
} // namespace
struct Store::Impl {
  Impl(const std::string& directory, Manifest identity, StoreFailureHook failure_hook, StoreOpenMode mode,
      std::optional<Genesis> initial = {})
    : manifest(std::move(identity)), hook(std::move(failure_hook)), genesis(std::move(initial)) {
    const auto encoded = encode_manifest(manifest);
    if (genesis) { validate_ledger_identity(manifest,*genesis); ledger = LedgerState{0,genesis->accounts}; }
    if (directory.empty()) throw std::invalid_argument("Raft directory is required");
    if (mode == StoreOpenMode::create_or_open) std::filesystem::create_directories(directory);
    rocksdb::Options options; options.create_if_missing = mode == StoreOpenMode::create_or_open;
    options.paranoid_checks = true; options.max_open_files = 128;
    options.write_buffer_size = 4*1024*1024; options.max_write_buffer_number = 2;
    checked(open_rocksdb(options,directory,db),"open Raft database");
    std::string stored; const auto read = db->Get(rocksdb::ReadOptions{},"meta/identity",&stored);
    if (read.IsNotFound()) {
      if (mode == StoreOpenMode::existing_only) throw std::runtime_error("Raft database is uninitialized");
      const std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(rocksdb::ReadOptions{}));
      iterator->SeekToFirst();
      if (iterator->Valid()) throw std::runtime_error("nonempty Raft database lacks manifest");
      checked(iterator->status(),"inspect Raft database");
      rocksdb::WriteBatch batch;
      checked(batch.Put("meta/identity",encoded),"stage Raft identity");
      checked(batch.Put("raft/state",encode_state_record(manifest,state)),"stage initial Raft state");
      if (genesis) {
        checked(batch.Put("ledger/genesis",encode_genesis(*genesis)),"stage ledger genesis");
        checked(batch.Put("ledger/state",encode_state(*ledger)),"stage initial application state");
      }
      sync(batch);
    } else {
      checked(read,"read Raft identity");
      if (decode_manifest(stored) != manifest) throw std::runtime_error("Raft database identity mismatch");
      checked(db->Get(rocksdb::ReadOptions{},"raft/state",&stored),"read Raft state");
      state = decode_state_record(manifest,stored);
      const auto saved_genesis = get("ledger/genesis");
      if (saved_genesis.has_value() != genesis.has_value()) throw std::runtime_error("Raft store ledger mode mismatch");
      if (genesis) {
        if (*saved_genesis != encode_genesis(*genesis)) throw std::runtime_error("Raft ledger genesis mismatch");
        const auto saved_state = get("ledger/state");
        if (!saved_state) throw std::runtime_error("missing Raft application state");
        ledger = decode_state(*saved_state);
        for (const auto& entry : state.log) if (!entry.command.empty()) (void)decode_transfer_command(entry.command);
        // Validate even orphaned receipt records; never silently ignore an invalid key.
        const std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(rocksdb::ReadOptions{}));
        for (iterator->Seek("receipt/"); iterator->Valid() && iterator->key().starts_with("receipt/"); iterator->Next()) {
          const auto receipt = decode_receipt(iterator->value().ToString());
          if (iterator->key().ToString() != receipt_key(receipt.transaction_id)) throw std::runtime_error("receipt key/identity mismatch");
          validate_receipt_origin(receipt,receipt.transaction_id,*ledger,state);
        }
        checked(iterator->status(),"scan Raft ledger receipts");
        audit_raft_application(*genesis,*ledger,state,[this](const Hash& id) { return find_receipt(id); });
        apply(); // Replay only the committed suffix before exposing the reopened store.
      } else if (get("ledger/state")) throw std::runtime_error("unexpected ledger state in core-only store");
    }
  }
  void sync(rocksdb::WriteBatch& batch) {
    rocksdb::WriteOptions options; options.sync = true; options.disableWAL = false;
    checked(db->Write(options,&batch),"synchronous Raft state write");
  }
  void healthy() const { if (failed) throw std::runtime_error("Raft store failed; reopen before use"); }
  void require_ledger() const { healthy(); if (!genesis) throw std::logic_error("Raft store has no ledger"); }
  std::optional<std::string> get(const std::string& key) const {
    std::string value; const auto status = db->Get(rocksdb::ReadOptions{},key,&value);
    if (status.IsNotFound()) return std::nullopt;
    checked(status,"read Raft ledger"); return value;
  }
  std::optional<LedgerReceipt> find_receipt(const Hash& id) const {
    const auto value = get(receipt_key(id)); if (!value) return std::nullopt;
    const auto receipt = decode_receipt(*value); validate_receipt_origin(receipt,id,*ledger,state); return receipt;
  }
  void apply() {
    require_ledger();
    while (ledger->applied_index < state.commit) {
      const auto plan = plan_committed_entry(*genesis,*ledger,state,ledger->applied_index+1,
        [this](const Hash& id) { return find_receipt(id); });
      rocksdb::WriteBatch batch;
      for (const auto& receipt : plan.new_receipts)
        checked(batch.Put(receipt_key(receipt.transaction_id),encode_receipt(receipt)),"stage Raft ledger receipt");
      checked(batch.Put("ledger/state",encode_state(plan.state)),"stage Raft application progress");
      if (hook) hook(StoreCrashPoint::before_apply);
      sync(batch); ledger = plan.state;
      if (hook) hook(StoreCrashPoint::after_apply);
    }
  }
  Manifest manifest;
  StoreFailureHook hook;
  std::unique_ptr<rocksdb::DB> db;
  mutable std::mutex mutex;
  State state;
  bool failed = false;
  std::optional<Genesis> genesis;
  std::optional<LedgerState> ledger;
};
Store::Store(std::string directory, Manifest manifest, StoreFailureHook hook, StoreOpenMode mode)
  : impl_(std::make_unique<Impl>(directory,std::move(manifest),std::move(hook),mode)) {}
Store::Store(std::string directory, Manifest manifest, Genesis genesis, StoreFailureHook hook, StoreOpenMode mode)
  : impl_(std::make_unique<Impl>(directory,std::move(manifest),std::move(hook),mode,std::move(genesis))) {}
Store::~Store() = default;
State Store::load() const { std::lock_guard lock(impl_->mutex); impl_->healthy(); return impl_->state; }
void Store::persist(const State& next) {
  std::lock_guard lock(impl_->mutex); impl_->healthy();
  validate_persisted_transition(impl_->manifest,impl_->state,next);
  if (next == impl_->state) return;
  if (impl_->genesis) {
    for (std::size_t i = 0; i < next.log.size(); ++i)
      if ((i >= impl_->state.log.size() || next.log[i] != impl_->state.log[i]) && !next.log[i].command.empty())
        (void)decode_transfer_command(next.log[i].command);
  }
  State staged = next; // Allocate before performing any write; publication is a move.
  const auto encoded = encode_state_record(impl_->manifest,staged);
  try {
    rocksdb::WriteBatch batch; checked(batch.Put("raft/state",encoded),"stage Raft state");
    if (impl_->hook) impl_->hook(StoreCrashPoint::before_write);
    impl_->sync(batch); impl_->state = std::move(staged);
    if (impl_->hook) impl_->hook(StoreCrashPoint::after_write);
  } catch (...) { impl_->failed = true; throw; }
}
Index Store::apply_committed() {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  try { impl_->apply(); return impl_->ledger->applied_index; }
  catch (...) { impl_->failed = true; throw; }
}
LedgerState Store::ledger_state() const {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger(); return *impl_->ledger;
}
std::optional<LedgerReceipt> Store::receipt(const Hash& id) const {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  try { return impl_->find_receipt(id); } catch (...) { impl_->failed = true; throw; }
}
std::optional<LedgerBatch> Store::batch(Index index) const {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  if (index == 0 || index > impl_->ledger->applied_index) return std::nullopt;
  try {
    const auto& command = impl_->state.log[index-1].command;
    if (command.empty()) return std::nullopt;
    return LedgerBatch{index,decode_transfer_command(command)};
  } catch (...) { impl_->failed = true; throw; }
}
} // namespace rollupx::raft
