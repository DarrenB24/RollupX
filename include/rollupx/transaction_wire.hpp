#pragma once
#include "rollupx/transaction.hpp"
#include "transaction.pb.h"
#include <string_view>

namespace rollupx {
// Wire serialization is not a signing format. Version 1 messages are bounded and
// reject unknown fields so unsupported semantics cannot be silently discarded.
inline constexpr std::size_t kMaxTransactionWireBytes = 512;
SignedTransaction transaction_from_proto(const v1::Transaction& message);
v1::Transaction transaction_to_proto(const SignedTransaction& transaction);
SignedTransaction parse_transaction_wire(std::string_view bytes);
std::string serialize_transaction_wire(const SignedTransaction& transaction);
Hash transaction_id_from_proto(const v1::TransactionId& message);
Address account_address_from_proto(const v1::AccountAddress& message);
} // namespace rollupx
