#include "rollupx/crypto.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace rollupx {
namespace {
// Keccak-f[1600], 24 rounds, little-endian lanes. Ethereum suffix 0x01, NOT SHA3 0x06.
constexpr std::array<std::uint64_t, 24> constants{
  0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
  0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
  0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
  0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
  0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
  0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
constexpr std::array<int, 25> rotation{0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14};
void permute(std::array<std::uint64_t, 25>& a) {
  for (const auto round : constants) {
    std::array<std::uint64_t, 5> c{}, d{};
    for (std::size_t x = 0; x < 5; ++x) {
      for (std::size_t y = 0; y < 5; ++y) c[x] ^= a[x + 5*y];
    }
    for (std::size_t x = 0; x < 5; ++x) d[x] = c[(x+4)%5] ^ std::rotl(c[(x+1)%5], 1);
    std::array<std::uint64_t, 25> b{};
    for (std::size_t x = 0; x < 5; ++x) for (std::size_t y = 0; y < 5; ++y)
      b[y + 5*((2*x + 3*y)%5)] = std::rotl(a[x+5*y] ^ d[x], rotation[x+5*y]);
    for (std::size_t x = 0; x < 5; ++x) for (std::size_t y = 0; y < 5; ++y)
      a[x+5*y] = b[x+5*y] ^ ((~b[(x+1)%5+5*y]) & b[(x+2)%5+5*y]);
    a[0] ^= round;
  }
}
} // namespace
Hash keccak256(std::span<const Byte> bytes) {
  constexpr std::size_t rate = 136;
  std::array<std::uint64_t, 25> state{};
  std::size_t offset = 0;
  for (const auto byte : bytes) {
    state[offset/8] ^= std::uint64_t{byte} << (8*(offset%8));
    if (++offset == rate) { permute(state); offset = 0; }
  }
  state[offset/8] ^= std::uint64_t{1} << (8*(offset%8));
  state[(rate-1)/8] ^= std::uint64_t{0x80} << (8*((rate-1)%8));
  permute(state);
  Hash result{};
  for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<Byte>(state[i/8] >> (8*(i%8)));
  return result;
}
std::string hex(std::span<const Byte> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result; result.reserve(bytes.size()*2);
  for (const auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::vector<Byte> unhex(std::string_view value) {
  if (value.starts_with("0x")) value.remove_prefix(2);
  if (value.size()%2 != 0) throw std::invalid_argument("hex requires pairs of digits");
  const auto digit = [](char c) -> Byte {
    if (c >= '0' && c <= '9') return static_cast<Byte>(c-'0');
    if (c >= 'a' && c <= 'f') return static_cast<Byte>(c-'a'+10);
    if (c >= 'A' && c <= 'F') return static_cast<Byte>(c-'A'+10);
    throw std::invalid_argument("invalid hex digit");
  };
  std::vector<Byte> result; result.reserve(value.size()/2);
  for (std::size_t i = 0; i < value.size(); i += 2)
    result.push_back(static_cast<Byte>((digit(value[i]) << 4) | digit(value[i+1])));
  return result;
}
} // namespace rollupx
