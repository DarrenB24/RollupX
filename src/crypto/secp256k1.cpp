#include "rollupx/crypto.hpp"
#include <algorithm>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <utility>

namespace rollupx {
namespace {
template<class T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
using Big = Owned<BIGNUM, BN_clear_free>;
using Point = Owned<EC_POINT, EC_POINT_free>;
using Pkey = Owned<EVP_PKEY, EVP_PKEY_free>;
void check(bool success) { if (!success) throw std::runtime_error("OpenSSL cryptographic operation failed"); }
Big big() { Big value(BN_new(), BN_clear_free); check(bool(value)); return value; }
Big integer(std::span<const Byte> bytes) {
  Big result(BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr), BN_clear_free);
  check(bool(result)); return result;
}
struct Curve {
  Owned<EC_GROUP, EC_GROUP_free> group{EC_GROUP_new_by_curve_name(NID_secp256k1), EC_GROUP_free};
  Owned<BN_CTX, BN_CTX_free> context{BN_CTX_new(), BN_CTX_free};
  Big order = big(), half = big();
  Curve() {
    check(bool(group) && bool(context));
    check(EC_GROUP_get_order(group.get(), order.get(), context.get()) == 1);
    check(BN_rshift1(half.get(), order.get()) == 1);
  }
  Point point() const { Point value(EC_POINT_new(group.get()), EC_POINT_free); check(bool(value)); return value; }
};
bool valid_scalar(const BIGNUM* value, const BIGNUM* order) {
  return !BN_is_negative(value) && !BN_is_zero(value) && BN_cmp(value, order) < 0;
}
bool canonical(const Signature& signature, const Curve& curve) {
  if (signature[64] > 1) return false;
  const auto r = integer(std::span(signature).first<32>());
  const auto s = integer(std::span(signature).subspan<32,32>());
  return valid_scalar(r.get(), curve.order.get()) && valid_scalar(s.get(), curve.order.get()) &&
    BN_cmp(s.get(), curve.half.get()) <= 0;
}
PublicKey encoded(const Curve& curve, const EC_POINT* point) {
  PublicKey result{};
  check(EC_POINT_point2oct(curve.group.get(), point, POINT_CONVERSION_UNCOMPRESSED,
    result.data(), result.size(), curve.context.get()) == result.size());
  return result;
}
Address public_address(const PublicKey& key) {
  const auto digest = keccak256(std::span(key).subspan<1>());
  Address result{}; std::copy(digest.end()-20, digest.end(), result.begin()); return result;
}
Pkey import_key(const PublicKey& pub, const BIGNUM* secret = nullptr) {
  Owned<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> builder(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
  check(bool(builder));
  check(OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "secp256k1", 0) == 1);
  check(OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub.size()) == 1);
  if (secret) check(OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, secret) == 1);
  Owned<OSSL_PARAM, OSSL_PARAM_free> parameters(OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
  Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free);
  check(bool(parameters) && bool(context));
  check(EVP_PKEY_fromdata_init(context.get()) == 1);
  EVP_PKEY* output = nullptr;
  const int result = EVP_PKEY_fromdata(context.get(), &output,
    secret ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, parameters.get());
  Pkey key(output, EVP_PKEY_free); check(result == 1 && bool(key)); return key;
}
std::vector<Byte> der_signature(const Signature& signature) {
  auto r = integer(std::span(signature).first<32>());
  auto s = integer(std::span(signature).subspan<32,32>());
  Owned<ECDSA_SIG, ECDSA_SIG_free> value(ECDSA_SIG_new(), ECDSA_SIG_free); check(bool(value));
  check(ECDSA_SIG_set0(value.get(), r.get(), s.get()) == 1);
  (void)r.release(); (void)s.release(); // ECDSA_SIG owns both on success.
  const int size = i2d_ECDSA_SIG(value.get(), nullptr); check(size > 0);
  std::vector<Byte> result(static_cast<std::size_t>(size));
  auto* cursor = result.data(); check(i2d_ECDSA_SIG(value.get(), &cursor) == size); return result;
}
} // namespace
PrivateKey::~PrivateKey() { OPENSSL_cleanse(value_.data(), value_.size()); }
PrivateKey::PrivateKey(PrivateKey&& other) noexcept : value_(other.value_) { OPENSSL_cleanse(other.value_.data(), other.value_.size()); }
PrivateKey& PrivateKey::operator=(PrivateKey&& other) noexcept {
  if (this != &other) { OPENSSL_cleanse(value_.data(), value_.size()); value_ = other.value_; OPENSSL_cleanse(other.value_.data(), other.value_.size()); }
  return *this;
}
PrivateKey PrivateKey::from_bytes(std::span<const Byte,32> bytes) {
  Curve curve; auto scalar = integer(bytes);
  if (!valid_scalar(scalar.get(), curve.order.get())) throw std::invalid_argument("private key outside secp256k1 range");
  Hash result{}; std::copy(bytes.begin(), bytes.end(), result.begin());
  PrivateKey key(result); OPENSSL_cleanse(result.data(), result.size()); return key;
}
PrivateKey PrivateKey::generate() {
  // RAND_priv_bytes uses OpenSSL's private DRBG. Never use a general-purpose PRNG.
  PrivateKey result(Hash{}); Curve curve;
  do { check(RAND_priv_bytes(result.value_.data(), static_cast<int>(result.value_.size())) == 1); }
  while (!valid_scalar(integer(result.value_).get(), curve.order.get()));
  return result;
}
PublicKey public_key(const PrivateKey& key) {
  Curve curve; auto secret = integer(key.bytes());
  if (!valid_scalar(secret.get(), curve.order.get())) throw std::invalid_argument("invalid or moved-from private key");
  BN_set_flags(secret.get(), BN_FLG_CONSTTIME);
  const auto point = curve.point();
  check(EC_POINT_mul(curve.group.get(), point.get(), secret.get(), nullptr, nullptr, curve.context.get()) == 1);
  return encoded(curve, point.get());
}
Address address(const PrivateKey& key) { return public_address(public_key(key)); }
std::optional<Address> recover_address(const Hash& digest, const Signature& signature) {
  Curve curve;
  if (!canonical(signature, curve)) return std::nullopt;
  auto r = integer(std::span(signature).first<32>()), s = integer(std::span(signature).subspan<32,32>());
  auto z = integer(digest);
  const auto R = curve.point(), Q = curve.point(), order_R = curve.point();
  if (EC_POINT_set_compressed_coordinates(curve.group.get(), R.get(), r.get(), signature[64], curve.context.get()) != 1) {
    ERR_clear_error(); return std::nullopt;
  }
  check(EC_POINT_mul(curve.group.get(), order_R.get(), nullptr, R.get(), curve.order.get(), curve.context.get()) == 1);
  if (EC_POINT_is_at_infinity(curve.group.get(), order_R.get()) != 1) return std::nullopt;
  auto inverse = big(), u1 = big(), u2 = big();
  check(BN_mod_inverse(inverse.get(), r.get(), curve.order.get(), curve.context.get()) != nullptr);
  check(BN_nnmod(z.get(), z.get(), curve.order.get(), curve.context.get()) == 1);
  check(BN_mod_sub(u1.get(), curve.order.get(), z.get(), curve.order.get(), curve.context.get()) == 1);
  check(BN_mod_mul(u1.get(), u1.get(), inverse.get(), curve.order.get(), curve.context.get()) == 1);
  check(BN_mod_mul(u2.get(), s.get(), inverse.get(), curve.order.get(), curve.context.get()) == 1);
  check(EC_POINT_mul(curve.group.get(), Q.get(), u1.get(), R.get(), u2.get(), curve.context.get()) == 1);
  if (EC_POINT_is_at_infinity(curve.group.get(), Q.get()) == 1) return std::nullopt;
  return public_address(encoded(curve, Q.get()));
}
bool verify_digest(const PublicKey& pub, const Hash& digest, const Signature& signature) {
  Curve curve;
  if (pub[0] != 4 || !canonical(signature, curve)) return false;
  const auto point = curve.point();
  if (EC_POINT_oct2point(curve.group.get(), point.get(), pub.data(), pub.size(), curve.context.get()) != 1) {
    ERR_clear_error(); return false;
  }
  const auto key = import_key(pub);
  Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
  check(bool(context)); check(EVP_PKEY_verify_init(context.get()) == 1);
  const auto der = der_signature(signature);
  const int result = EVP_PKEY_verify(context.get(), der.data(), der.size(), digest.data(), digest.size());
  check(result >= 0);
  // EVP verifies r/s; also enforce that the recovery bit selects the same identity.
  return result == 1 && recover_address(digest, signature) == public_address(pub);
}
Signature sign_digest(const PrivateKey& key, const Hash& digest) {
  Curve curve; const auto pub = public_key(key); auto secret = integer(key.bytes());
  BN_set_flags(secret.get(), BN_FLG_CONSTTIME);
  const auto provider_key = import_key(pub, secret.get());
  Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new(provider_key.get(), nullptr), EVP_PKEY_CTX_free);
  check(bool(context)); check(EVP_PKEY_sign_init(context.get()) == 1);
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::array<Byte, 80> der{}; std::size_t size = der.size();
    // Sign the already-computed 32-byte digest; EVP_PKEY_sign does not hash it again.
    check(EVP_PKEY_sign(context.get(), der.data(), &size, digest.data(), digest.size()) == 1);
    const auto* cursor = der.data();
    Owned<ECDSA_SIG, ECDSA_SIG_free> decoded(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(size)), ECDSA_SIG_free);
    check(bool(decoded) && cursor == der.data()+size);
    const BIGNUM *r = nullptr, *s = nullptr; ECDSA_SIG_get0(decoded.get(), &r, &s);
    auto low_s = big(); check(BN_copy(low_s.get(), s) != nullptr);
    if (BN_cmp(s, curve.half.get()) > 0) check(BN_sub(low_s.get(), curve.order.get(), s) == 1);
    Signature result{};
    check(BN_bn2binpad(r, result.data(), 32) == 32);
    check(BN_bn2binpad(low_s.get(), result.data()+32, 32) == 32);
    for (Byte parity = 0; parity < 2; ++parity) {
      result[64] = parity;
      if (recover_address(digest, result) == public_address(pub)) return result;
    }
    // Extremely rare x >= group order needs recovery ID 2/3; retry for Ethereum.
  }
  throw std::runtime_error("unable to produce an Ethereum-recoverable signature");
}
} // namespace rollupx
