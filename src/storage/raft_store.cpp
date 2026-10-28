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
  std::optional<SnapshotStage> staged() {
    try {
      const auto value = get("snapshot/staging");
      return value ? std::optional{decode_snapshot_stage(*value)} : std::nullopt;
    } catch (...) { failed = true; throw; }
  }
  void check_receipts(const std::map<Hash,LedgerReceipt>& expected) const {
    const std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(rocksdb::ReadOptions{}));
    for (iterator->Seek("receipt/"); iterator->Valid() && iterator->key().starts_with("receipt/"); iterator->Next()) {
      const auto receipt = decode_receipt(iterator->value().ToString());
      const auto found = expected.find(receipt.transaction_id);
      if (iterator->key().ToString() != receipt_key(receipt.transaction_id) || found == expected.end() || found->second != receipt)
        throw std::runtime_error("stored receipt conflicts with snapshot history");
    }
    checked(iterator->status(),"audit snapshot receipts");
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
std::string Store::create_snapshot() {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  if (impl_->state.commit == 0 || impl_->ledger->applied_index != impl_->state.commit)
    throw std::invalid_argument("checkpoint requires nonempty fully applied commitment");
  try {
    const auto snapshot = make_snapshot(impl_->manifest,*impl_->genesis,impl_->state,*impl_->ledger,
      [this](const Hash& id) { return impl_->find_receipt(id); });
    impl_->check_receipts(snapshot.receipts);
    const auto encoded = encode_snapshot(impl_->manifest,*impl_->genesis,snapshot);
    rocksdb::WriteBatch batch; checked(batch.Put("snapshot/local",encoded),"stage local checkpoint");
    if (impl_->hook) impl_->hook(StoreCrashPoint::before_checkpoint);
    impl_->sync(batch);
    if (impl_->hook) impl_->hook(StoreCrashPoint::after_checkpoint);
    return encoded;
  } catch (...) { impl_->failed = true; throw; }
}
std::optional<std::string> Store::local_snapshot() const {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  try {
    const auto encoded = impl_->get("snapshot/local");
    if (encoded) {
      const auto snapshot = decode_snapshot(impl_->manifest,*impl_->genesis,*encoded);
      if (snapshot.prefix.commit > impl_->ledger->applied_index ||
          !std::equal(snapshot.prefix.log.begin(),snapshot.prefix.log.end(),impl_->state.log.begin()))
        throw std::runtime_error("local checkpoint differs from active committed history");
    }
    return encoded;
  } catch (...) { impl_->failed = true; throw; }
}
SnapshotProgress Store::stage_snapshot(const SnapshotChunk& chunk) {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  const auto current = impl_->staged();
  const auto next = append_snapshot_chunk(current,chunk); // Caller errors never poison active state.
  if (next.complete()) (void)decode_snapshot(impl_->manifest,*impl_->genesis,next.data);
  const SnapshotProgress progress{next.id,next.total_bytes,next.data.size(),next.complete()};
  if (current && current->id == next.id && current->data == next.data) return progress;
  const auto encoded = encode_snapshot_stage(next);
  try {
    rocksdb::WriteBatch batch; checked(batch.Put("snapshot/staging",encoded),"stage snapshot chunk");
    if (impl_->hook) impl_->hook(StoreCrashPoint::before_stage);
    impl_->sync(batch);
    if (impl_->hook) impl_->hook(StoreCrashPoint::after_stage);
    return progress;
  } catch (...) { impl_->failed = true; throw; }
}
std::optional<SnapshotProgress> Store::staged_snapshot() const {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  const auto staged = impl_->staged();
  if (!staged) return {};
  return SnapshotProgress{staged->id,staged->total_bytes,staged->data.size(),staged->complete()};
}
void Store::discard_staged_snapshot() {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  try {
    rocksdb::WriteBatch batch; checked(batch.Delete("snapshot/staging"),"discard staged snapshot"); impl_->sync(batch);
  } catch (...) { impl_->failed = true; throw; }
}
bool Store::publish_staged_snapshot() {
  std::lock_guard lock(impl_->mutex); impl_->require_ledger();
  const auto staged = impl_->staged();
  if (!staged || !staged->complete()) throw std::invalid_argument("snapshot staging is incomplete");
  // Decode again after reopening or intervening disk writes, before any publication.
  LedgerSnapshot snapshot;
  try { snapshot = decode_snapshot(impl_->manifest,*impl_->genesis,staged->data); }
  catch (...) { impl_->failed = true; throw; }
  const auto index = snapshot.prefix.commit;
  if (snapshot.prefix.term > impl_->state.term || index < impl_->state.commit || index < impl_->ledger->applied_index)
    throw std::invalid_argument("snapshot would regress progress or exceed the persisted current term");
  if (!std::equal(impl_->state.log.begin(),impl_->state.log.begin()+impl_->state.commit,snapshot.prefix.log.begin()))
    throw std::invalid_argument("snapshot conflicts with local committed prefix");
  State next = impl_->state; next.log = snapshot.prefix.log; next.commit = index;
  if (index <= impl_->state.log.size() && impl_->state.log[index-1].term == snapshot.prefix.term) {
    if (!std::equal(next.log.begin(),next.log.end(),impl_->state.log.begin()))
      throw std::invalid_argument("matching snapshot boundary has a contradictory prefix");
    next.log.insert(next.log.end(),impl_->state.log.begin()+index,impl_->state.log.end());
  }
  validate_persisted_transition(impl_->manifest,impl_->state,next);
  const bool changed = next != impl_->state || snapshot.ledger != *impl_->ledger;
  const auto encoded_state = encode_state_record(impl_->manifest,next);
  const auto encoded_ledger = encode_state(snapshot.ledger);
  try {
    impl_->check_receipts(snapshot.receipts);
    rocksdb::WriteBatch batch;
    checked(batch.Put("raft/state",encoded_state),"publish snapshot Raft state");
    checked(batch.Put("ledger/state",encoded_ledger),"publish snapshot ledger state");
    for (const auto& [id,receipt] : snapshot.receipts)
      checked(batch.Put(receipt_key(id),encode_receipt(receipt)),"publish snapshot receipt");
    checked(batch.Put("snapshot/local",staged->data),"publish snapshot generation");
    checked(batch.Delete("snapshot/staging"),"finish snapshot staging");
    if (impl_->hook) impl_->hook(StoreCrashPoint::before_publish);
    impl_->sync(batch); impl_->state = std::move(next); impl_->ledger = std::move(snapshot.ledger);
    if (impl_->hook) impl_->hook(StoreCrashPoint::after_publish);
    return changed;
  } catch (...) { impl_->failed = true; throw; }
}
} // namespace rollupx::raft
