#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rollupx {
using Byte = std::uint8_t;
using Hash = std::array<Byte, 32>;
using Address = std::array<Byte, 20>;
using Signature = std::array<Byte, 65>;
using PublicKey = std::array<Byte, 65>;

std::string hex(std::span<const Byte> bytes);
std::vector<Byte> unhex(std::string_view value);
template<std::size_t N> std::array<Byte, N> fixed_hex(std::string_view value) {
  if (value.starts_with("0x")) value.remove_prefix(2);
  if (value.size() != 2*N) throw std::invalid_argument("incorrect hex field length");
  const auto bytes = unhex(value);
  std::array<Byte, N> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}
Hash keccak256(std::span<const Byte> bytes);

// Move-only, zeroized on destruction. Export is explicit; callers own exported copies.
class PrivateKey {
 public:
  static PrivateKey generate();
  static PrivateKey from_bytes(std::span<const Byte, 32> bytes);
  ~PrivateKey();
  PrivateKey(const PrivateKey&) = delete;
  PrivateKey& operator=(const PrivateKey&) = delete;
  PrivateKey(PrivateKey&& other) noexcept;
  PrivateKey& operator=(PrivateKey&& other) noexcept;
  [[nodiscard]] std::span<const Byte, 32> bytes() const { return value_; }
 private:
  explicit PrivateKey(const Hash& value) : value_(value) {}
  Hash value_;
};
PublicKey public_key(const PrivateKey& key);
Address address(const PrivateKey& key);
Signature sign_digest(const PrivateKey& key, const Hash& digest);
// Malformed signatures return nullopt. OpenSSL operational failures throw.
std::optional<Address> recover_address(const Hash& digest, const Signature& signature);
bool verify_digest(const PublicKey& key, const Hash& digest, const Signature& signature);
} // namespace rollupx
