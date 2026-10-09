#include "rollupx/transaction.hpp"
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <openssl/evp.h>

namespace {
using namespace rollupx;
PrivateKey key_one() { Hash value{}; value.back() = 1; return PrivateKey::from_bytes(value); }
TEST(Hex, StrictLengthAndDigits) {
  EXPECT_EQ(hex(unhex("0x00aBff")), "00abff");
  EXPECT_THROW(unhex("1"), std::invalid_argument);
  EXPECT_THROW(unhex("zz"), std::invalid_argument);
  EXPECT_THROW(fixed_hex<20>("00"), std::invalid_argument);
}
TEST(Keccak, EthereumKnownAnswersAndNotSha3) {
  EXPECT_EQ(hex(keccak256({})), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  const std::array<Byte,3> abc{'a','b','c'};
  EXPECT_EQ(hex(keccak256(abc)), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
  // Published EIP-155 signing preimage/hash, independent of our transaction codec.
  const auto preimage = unhex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080");
  EXPECT_EQ(hex(keccak256(preimage)), "daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53");
}
TEST(Keccak, IndependentOpenSslOracleAcrossBlockBoundaries) {
  std::unique_ptr<EVP_MD, decltype(&EVP_MD_free)> algorithm(EVP_MD_fetch(nullptr, "KECCAK-256", nullptr), EVP_MD_free);
  if (!algorithm) GTEST_SKIP() << "OpenSSL provider lacks Keccak (known-answer tests still required)";
  for (const std::size_t size : {0,1,31,32,135,136,137,271,272,273,1024,8192}) {
    std::vector<Byte> input(size);
    for (std::size_t i = 0; i < size; ++i) input[i] = static_cast<Byte>(i*17+3);
    Hash expected{}; unsigned length = 0;
    ASSERT_EQ(EVP_Digest(input.data(), input.size(), expected.data(), &length, algorithm.get(), nullptr), 1);
    ASSERT_EQ(length, 32U); EXPECT_EQ(keccak256(input), expected) << "length=" << size;
  }
}
TEST(Keccak, FrozenRateBoundaryVectors) {
  // Pattern byte[i]=(17*i+3) mod 256, independently generated with OpenSSL 3.6.1.
  // Remain mandatory on OpenSSL 3.0 hosts without a KECCAK-256 provider.
  const std::pair<std::size_t, const char*> cases[] = {
    {135, "67ac26d3e79a18e88f212385a7bad88e6f9cfd25b5bfadbc2c4d0cf95457e613"},
    {136, "e0ab45aba084eb3ddec8f785b6993a3555833dd941733756a685525a84fe012c"},
    {137, "54e8f24f3543727f3825a7e1dd0e9c4d01b5254f5af2ae0fa3644212495d92b2"},
    {272, "57e0d7506f2c4eb5f09f7318721fbd04852849690c86ced13e7642fb17035478"}};
  for (const auto& [size, expected] : cases) {
    std::vector<Byte> input(size);
    for (std::size_t i = 0; i < size; ++i) input[i] = static_cast<Byte>(i*17+3);
    EXPECT_EQ(hex(keccak256(input)), expected);
  }
}
TEST(Signature, KnownEthereumAddressAndEip155RecoveryVector) {
  auto one = key_one();
  EXPECT_EQ(hex(address(one)), "7e5f4552091a69125d5dfcb7b8c2659029395bdf");
  Hash scalar{}; scalar.fill(0x46); const auto key = PrivateKey::from_bytes(scalar);
  const auto digest = fixed_hex<32>("daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53");
  const auto sig = fixed_hex<65>("28ef61340bd939bc2195fe537567866003e1a15d3c71ff63e1590620aa636276"
    "67cbe9d8997f761aecb703304b3800ccf555c9f3dc64214b297fb1966a3b6d83" "00");
  EXPECT_EQ(recover_address(digest, sig), address(key));
  EXPECT_TRUE(verify_digest(public_key(key), digest, sig));
}
TEST(Signature, SignsRawDigestsAndRejectsTamperingAndWrongRecoveryBit) {
  const auto key = key_one(); const Hash digest = keccak256(unhex("123456"));
  const auto signature = sign_digest(key, digest);
  EXPECT_TRUE(verify_digest(public_key(key), digest, signature));
  EXPECT_EQ(recover_address(digest, signature), address(key));
  auto changed = digest; changed[0] ^= 1;
  EXPECT_FALSE(verify_digest(public_key(key), changed, signature));
  auto flipped = signature; flipped[64] ^= 1;
  EXPECT_FALSE(verify_digest(public_key(key), digest, flipped));
  flipped = signature; flipped[0] ^= 1;
  EXPECT_FALSE(verify_digest(public_key(key), digest, flipped));
}
TEST(Signature, RejectsOutOfRangeAndHighS) {
  const auto key = key_one(); const Hash digest{}; const auto signature = sign_digest(key, digest);
  auto invalid = signature; invalid[64] = 2; EXPECT_FALSE(recover_address(digest, invalid));
  invalid = signature; std::fill_n(invalid.begin(), 32, 0); EXPECT_FALSE(recover_address(digest, invalid));
  invalid = signature; std::fill_n(invalid.begin()+32, 32, 0); EXPECT_FALSE(recover_address(digest, invalid));
  const auto order = fixed_hex<32>("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  invalid = signature; std::copy(order.begin(), order.end(), invalid.begin()); EXPECT_FALSE(recover_address(digest, invalid));
  invalid = signature; std::copy(order.begin(), order.end(), invalid.begin()+32); --invalid[63];
  EXPECT_FALSE(recover_address(digest, invalid));
  Hash zero{}; EXPECT_THROW(PrivateKey::from_bytes(zero), std::invalid_argument);
  EXPECT_THROW(PrivateKey::from_bytes(order), std::invalid_argument);
}
TEST(Signature, GenerationMovesAndConcurrentVerification) {
  auto key = PrivateKey::generate(); const auto expected = address(key);
  auto moved = std::move(key); EXPECT_EQ(address(moved), expected);
  EXPECT_THROW(address(key), std::invalid_argument);
  const Hash digest{}; const auto signature = sign_digest(moved, digest);
  std::vector<std::future<bool>> jobs;
  for (int i = 0; i < 8; ++i) jobs.push_back(std::async(std::launch::async, [&] {
    for (int j = 0; j < 10; ++j) if (recover_address(digest, signature) != expected) return false;
    return true;
  }));
  for (auto& job : jobs) EXPECT_TRUE(job.get());
}
TEST(Transaction, CanonicalEncodingEnvelopeAndTampering) {
  const auto key = key_one();
  Transfer transfer{31337, address(key), fixed_hex<20>("3535353535353535353535353535353535353535"), 42, 7};
  const auto payload = encode_payload(transfer);
  EXPECT_EQ(hex(payload), "524f4c4c5550585f54585f56310000000000007a69"
    "7e5f4552091a69125d5dfcb7b8c2659029395bdf3535353535353535353535353535353535353535"
    "000000000000002a0000000000000007");
  EXPECT_EQ(decode_payload(payload), transfer);
  EXPECT_EQ(hex(keccak256(payload)), "cf8254a507a0678e1935b6cf15cb070964bfae917a8ce86baacf585d93ab5e7a");
  // Public test scalar d=1 and ephemeral k=1: r=G.x, s=(digest+r) mod n.
  // Never use fixed nonces for real signatures. Production signing uses OpenSSL DRBG.
  const auto golden_signature = fixed_hex<65>("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    "4940bb24017d233a6ed61964e4521211acacce85f912710944ef7b2bda6d34d1" "00");
  EXPECT_TRUE(verify_digest(public_key(key), keccak256(payload), golden_signature));
  EXPECT_EQ(authenticate(SignedTransaction{keccak256(payload), payload, golden_signature}), transfer);
  const auto tx = sign_transfer(key, transfer);
  EXPECT_EQ(decode_transaction(encode_transaction(tx)), tx);
  EXPECT_EQ(authenticate(tx), transfer);
  auto changed = tx; changed.payload[61] ^= 1; EXPECT_THROW(authenticate(changed), std::invalid_argument);
  changed.id = keccak256(changed.payload); EXPECT_THROW(authenticate(changed), std::invalid_argument);
  changed = tx; changed.id[0] ^= 1; EXPECT_THROW(authenticate(changed), std::invalid_argument);
  changed = tx; changed.payload[0] ^= 1; EXPECT_THROW(authenticate(changed), std::invalid_argument);
  changed = tx; changed.signature[64] = 27; EXPECT_THROW(authenticate(changed), std::invalid_argument);
  const auto envelope = encode_transaction(tx);
  EXPECT_THROW(decode_transaction(std::span(envelope).first<173>()), std::invalid_argument);
  std::vector<Byte> extra(envelope.begin(), envelope.end()); extra.push_back(0);
  EXPECT_THROW(decode_transaction(extra), std::invalid_argument);
  transfer.sender = transfer.recipient; EXPECT_THROW(sign_transfer(key, transfer), std::invalid_argument);
}
TEST(Transaction, EveryEnvelopeByteIsAuthenticated) {
  const auto key = key_one();
  const auto transaction = sign_transfer(key, Transfer{31337, address(key), fixed_hex<20>("3535353535353535353535353535353535353535"), 1, 0});
  const auto bytes = encode_transaction(transaction);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto changed = bytes; changed[i] ^= 1;
    EXPECT_THROW(decode_transaction(changed), std::invalid_argument) << "byte " << i;
  }
}
} // namespace
