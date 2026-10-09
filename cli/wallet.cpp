#include "rollupx/transaction.hpp"
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <iostream>
#include <openssl/crypto.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace rollupx;
class File {
 public:
  explicit File(int descriptor) : descriptor_(descriptor) {
    if (descriptor < 0) throw std::runtime_error("cannot open key file (must be a new file for keygen)");
  }
  ~File() { ::close(descriptor_); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  int get() const { return descriptor_; }
 private:
  int descriptor_;
};
struct SecretBuffer {
  Hash value{};
  ~SecretBuffer() { OPENSSL_cleanse(value.data(), value.size()); }
};
PrivateKey read_key(const char* path) {
  File file(::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat info{};
  if (::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != 32 ||
      info.st_uid != ::getuid() || (info.st_mode & 077) != 0)
    throw std::runtime_error("key file must be a 32-byte regular file owned by you with mode 0600");
  SecretBuffer buffer; std::size_t received = 0;
  while (received < buffer.value.size()) {
    const auto count = ::read(file.get(), buffer.value.data()+received, buffer.value.size()-received);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("cannot read complete key file");
    received += static_cast<std::size_t>(count);
  }
  return PrivateKey::from_bytes(buffer.value);
}
void write_key(const char* path, const PrivateKey& key) {
  File file(::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  std::size_t written = 0;
  while (written < key.bytes().size()) {
    const auto count = ::write(file.get(), key.bytes().data()+written, key.bytes().size()-written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("key write failed; remove incomplete file before retrying");
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(file.get()) != 0) throw std::runtime_error("key fsync failed");
}
std::uint64_t number(std::string_view input) {
  std::uint64_t result = 0;
  const auto [end, error] = std::from_chars(input.data(), input.data()+input.size(), result);
  if (error != std::errc{} || end != input.data()+input.size() || input.empty())
    throw std::invalid_argument("expected uint64 decimal integer");
  return result;
}
void usage() {
  std::cerr << "rollupx-wallet keygen KEYFILE\nrollupx-wallet address KEYFILE\n"
    "rollupx-wallet sign KEYFILE CHAIN_ID RECIPIENT_HEX AMOUNT NONCE\n"
    "rollupx-wallet verify TRANSACTION_HEX\n"
    "Offline only. Key files are raw 32-byte secrets, created exclusively with mode 0600.\n";
}
} // namespace
int main(int argc, char* argv[]) {
  try {
    if (argc < 2) { usage(); return 2; }
    const std::string_view command(argv[1]);
    if (command == "keygen" && argc == 3) {
      const auto key = PrivateKey::generate(); write_key(argv[2], key);
      std::cout << hex(address(key)) << '\n'; return 0;
    }
    if (command == "address" && argc == 3) {
      std::cout << hex(address(read_key(argv[2]))) << '\n'; return 0;
    }
    if (command == "sign" && argc == 7) {
      const auto key = read_key(argv[2]);
      const Transfer transfer{number(argv[3]), address(key), fixed_hex<20>(argv[4]), number(argv[5]), number(argv[6])};
      if (transfer.chain_id == 0 || transfer.amount == 0 || transfer.recipient == Address{} || transfer.recipient == transfer.sender)
        throw std::invalid_argument("chain/amount must be positive and recipient must be nonzero and different");
      std::cout << hex(encode_transaction(sign_transfer(key, transfer))) << '\n'; return 0;
    }
    if (command == "verify" && argc == 3) {
      std::string_view input(argv[2]);
      if (input.starts_with("0x")) input.remove_prefix(2);
      if (input.size() != 2*kTransactionBytes) throw std::invalid_argument("expected 174-byte transaction envelope");
      const auto tx = decode_transaction(unhex(input)); const auto transfer = decode_payload(tx.payload);
      std::cout << "id=" << hex(tx.id) << " sender=" << hex(transfer.sender) << " recipient=" << hex(transfer.recipient)
        << " chain_id=" << transfer.chain_id << " amount=" << transfer.amount << " nonce=" << transfer.nonce
        << " signature_valid=true (balance/nonce admission not checked)\n"; return 0;
    }
    usage(); return 2;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; return 1; }
}
