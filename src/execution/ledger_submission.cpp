#include "rollupx/ledger_submission.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace rollupx {
raft::ProposalCheck ledger_proposal_policy(Genesis genesis, LedgerStateReader state, ReceiptLookup receipt) {
  (void)encode_genesis(genesis);
  if (!state || !receipt) throw std::invalid_argument("ledger admission requires state and receipt readers");
  return [genesis=std::move(genesis),state=std::move(state),receipt=std::move(receipt)](
      const raft::State& consensus, std::string_view command) -> std::optional<std::string> {
    std::vector<SignedTransaction> transactions;
    try { transactions = decode_transfer_command(command); }
    catch (const std::invalid_argument&) { return "malformed_command"; }
    if (transactions.size() != 1) return "single_transfer_required";
    const auto& transaction = transactions.front();
    const auto applied = state();
    validate_recovery_state(genesis,applied,consensus.commit);
    if (applied.applied_index != consensus.commit) throw std::runtime_error("admission state is behind committed application");
    if (const auto prior = receipt(transaction.id)) {
      validate_receipt_origin(*prior,transaction.id,applied,consensus); return "duplicate";
    }
    if (consensus.log.size() != consensus.commit) return "busy";
    if (consensus.log.size() >= raft::kMaxLogEntries) return "log_full";
    std::size_t bytes = command.size();
    for (const auto& entry : consensus.log) {
      if (entry.command.size() > raft::kMaxLogBytes-bytes) return "log_full";
      bytes += entry.command.size();
    }
    const auto result = execute_transfer(applied.accounts,genesis.chain_id,authenticate(transaction)).result;
    if (result != TransferResult::applied) return std::string(result_name(result));
    return {};
  };
}
struct LedgerSubmission::Impl {
  Impl(raft::Runtime::Handle handle, ReceiptLookup reader, SubmissionOptions value)
    : runtime(std::move(handle)), receipt(std::move(reader)), options(value) {
    if (!receipt || options.max_calls == 0 || options.max_calls > 64 || options.max_duration.count() < 1 ||
        options.max_duration.count() > 10000 || options.poll_interval.count() < 1 || options.poll_interval.count() > 50)
      throw std::invalid_argument("invalid ledger submission options");
  }
  raft::Runtime::Handle runtime;
  ReceiptLookup receipt;
  SubmissionOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  SubmissionStatus stats;
  std::exception_ptr error;
};
LedgerSubmission::LedgerSubmission(raft::Runtime::Handle runtime, ReceiptLookup receipt, SubmissionOptions options)
  : impl_(std::make_unique<Impl>(std::move(runtime),std::move(receipt),options)) {}
LedgerSubmission::~LedgerSubmission() = default;
SubmissionResult LedgerSubmission::submit(const SignedTransaction& transaction, std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& cancelled) {
  deadline = std::min(deadline,std::chrono::steady_clock::now()+impl_->options.max_duration);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stats.closed) return {impl_->stats.failed ? SubmissionCode::failed : SubmissionCode::unavailable};
    if (impl_->stats.active == impl_->options.max_calls) { ++impl_->stats.backpressure; return {SubmissionCode::busy}; }
    ++impl_->stats.active;
  }
  struct Release { Impl& impl; ~Release() { std::lock_guard lock(impl.mutex); --impl.stats.active; } } release{*impl_};
  try {
    const auto interrupted = [&]() -> std::optional<SubmissionCode> {
      if (std::chrono::steady_clock::now() >= deadline) return SubmissionCode::deadline;
      if (cancelled && cancelled()) return SubmissionCode::cancelled;
      { std::lock_guard lock(impl_->mutex);
        if (impl_->stats.closed) return impl_->stats.failed ? SubmissionCode::failed : SubmissionCode::unavailable; }
      const auto status = impl_->runtime.status();
      if (status.failed) return SubmissionCode::failed;
      if (status.stopped) return SubmissionCode::unavailable;
      return {};
    };
    if (const auto reason = interrupted()) return {*reason};
    try { (void)authenticate(transaction); }
    catch (const std::invalid_argument&) { return {SubmissionCode::invalid}; }
    const auto read = [&]() -> std::optional<LedgerReceipt> {
      auto receipt = impl_->receipt(transaction.id);
      if (receipt) {
        // A callback returning a corrupt/wrong record is a storage failure, never
        // a successful retry. The actual Store additionally audits its origin.
        (void)encode_receipt(*receipt);
        if (receipt->transaction_id != transaction.id) throw std::runtime_error("receipt identity mismatch");
      }
      return receipt;
    };
    const auto confirmed = [&](const LedgerReceipt& receipt) -> SubmissionResult {
      if (const auto reason = interrupted()) return {*reason};
      return {SubmissionCode::confirmed,receipt};
    };
    if (const auto prior = read()) return confirmed(*prior);
    const auto command = encode_transfer_command({transaction});
    if (const auto reason = interrupted()) return {*reason};
    auto ticket = impl_->runtime.propose(command);
    if (ticket.admission == raft::Admission::full) return {SubmissionCode::busy};
    if (ticket.admission == raft::Admission::invalid) return {SubmissionCode::invalid};
    if (ticket.admission != raft::Admission::queued) return {SubmissionCode::unavailable};
    struct Cancel { raft::Ticket& ticket; ~Cancel() { ticket.cancel(); } } cancel{ticket};
    std::optional<raft::EventResult> result;
    for (;;) {
      if (const auto reason = interrupted()) return {*reason};
      if (!result && ticket.result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        result = ticket.result.get();
        if (result->completion == raft::Completion::failed) return {SubmissionCode::failed};
        if (result->completion != raft::Completion::processed && result->completion != raft::Completion::rejected)
          return {SubmissionCode::unavailable};
      }
      if (const auto prior = read()) return confirmed(*prior);
      if (result && result->completion == raft::Completion::rejected) {
        if (!result->policy_rejection) return {SubmissionCode::unavailable,{},"leader with current quorum required"};
        const auto& reason = *result->policy_rejection;
        if (reason == "duplicate") throw std::runtime_error("admission observed a missing durable receipt");
        return {reason == "busy" || reason == "log_full" ? SubmissionCode::busy : SubmissionCode::rejected,{},reason};
      }
      if (result) {
        const auto status = impl_->runtime.status();
        if (status.term != result->status.term || status.role != raft::Role::leader)
          return {SubmissionCode::unavailable,{},"leadership changed; retry by transaction ID"};
      }
      std::unique_lock lock(impl_->mutex);
      impl_->wake.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+impl_->options.poll_interval),
        [&] { return impl_->stats.closed; });
    }
  } catch (...) {
    std::lock_guard lock(impl_->mutex); if (!impl_->error) impl_->error = std::current_exception();
    impl_->stats.failed = true; impl_->stats.closed = true; impl_->wake.notify_all(); return {SubmissionCode::failed};
  }
}
void LedgerSubmission::close() { std::lock_guard lock(impl_->mutex); impl_->stats.closed = true; impl_->wake.notify_all(); }
SubmissionStatus LedgerSubmission::status() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
std::exception_ptr LedgerSubmission::failure() const { std::lock_guard lock(impl_->mutex); return impl_->error; }
} // namespace rollupx
