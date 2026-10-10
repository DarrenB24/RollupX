#include "rollupx/transaction_wire.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
  using namespace rollupx;
  try {
    if (argc != 3 || (std::string_view(argv[1]) != "encode" && std::string_view(argv[1]) != "decode")) {
      std::cerr << "rollupx-wire encode SIGNED_ENVELOPE_HEX\nrollupx-wire decode PROTOBUF_HEX\n"
        "Offline format conversion and authentication only; does not submit or commit.\n";
      return 2;
    }
    std::string_view input(argv[2]);
    if (input.starts_with("0x")) input.remove_prefix(2);
    if (std::string_view(argv[1]) == "encode") {
      if (input.size() != 2*kTransactionBytes) throw std::invalid_argument("expected 174-byte signed envelope");
      const auto bytes = serialize_transaction_wire(decode_transaction(unhex(input)));
      std::cout << hex({reinterpret_cast<const Byte*>(bytes.data()), bytes.size()}) << '\n';
    } else {
      if (input.empty() || input.size() > 2*kMaxTransactionWireBytes)
        throw std::invalid_argument("protobuf transaction must contain 1 to 512 bytes");
      const auto bytes = unhex(input);
      const auto transaction = parse_transaction_wire({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
      std::cout << hex(encode_transaction(transaction)) << '\n';
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
