#pragma once
#include "rollupx/raft_application.hpp"
#include "rollupx/raft_runtime.hpp"

namespace rollupx {
using LedgerStateReader = std::function<LedgerState()>;
// Conservative initial sequencing: one single-transfer command may be outstanding.
// Both readers must refer to the same audited ledger as Runtime's apply callback.
raft::ProposalCheck ledger_proposal_policy(Genesis genesis, LedgerStateReader state, ReceiptLookup receipt);

enum class SubmissionCode { confirmed, rejected, invalid, busy, unavailable, deadline, cancelled, failed };
struct SubmissionResult {
  SubmissionCode code;
  std::optional<LedgerReceipt> receipt = {}; // Present ONLY for durable confirmation.
  std::string reason = {}; // Admission rejection is not a durable rejection receipt.
};
struct SubmissionOptions {
  std::size_t max_calls = 16;
  std::chrono::milliseconds max_duration{2000};
  std::chrono::milliseconds poll_interval{5};
};
struct SubmissionStatus { std::size_t active = 0; std::uint64_t backpressure = 0; bool closed = false, failed = false; };
// Requires Runtime configured with ledger_proposal_policy. Receipt reads must be
// thread-safe, return only applied/durable records, and finish in bounded time.
// This class does not make an arbitrary callback durable or enable the node API.
class LedgerSubmission {
 public:
  LedgerSubmission(raft::Runtime::Handle runtime, ReceiptLookup receipt, SubmissionOptions options = {});
  ~LedgerSubmission();
  LedgerSubmission(const LedgerSubmission&) = delete;
  LedgerSubmission& operator=(const LedgerSubmission&) = delete;
  SubmissionResult submit(const SignedTransaction& transaction, std::chrono::steady_clock::time_point deadline,
    const std::function<bool()>& cancelled = {});
  void close(); // Ends waits; cannot roll back an already-started append/application.
  SubmissionStatus status() const;
  std::exception_ptr failure() const;
  // Caller must finish all submit calls before destruction; readers outlive calls.
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rollupx
