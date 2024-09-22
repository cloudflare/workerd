#include "dh.h"

#include <workerd/io/io-context.h>

#include <openssl/bn.h>
#include <openssl/dh.h>

#include <kj/one-of.h>
#include <kj/string.h>

#if WORKERD_BSSL_NEED_DH_PRIMES
#include <workerd/api/crypto/dh-primes.h>
#endif  // WORKERD_BSSL_NEED_DH_PRIMES

#if !_WIN32
#include <strings.h>
#endif

namespace workerd::api {

namespace {

// Maximum DH prime size, adapted from boringssl.
constexpr int OPENSSL_DH_MAX_MODULUS_BITS = 10000;

// Returns a function that can be used to create an instance of a standardized
// Diffie-Hellman group.
BIGNUM* (*findDiffieHellmanGroup(const char* name))(BIGNUM*) {
#if _WIN32
#define V(n, p)                                                                                    \
  if (_strnicmp(name, n, 7) == 0) {                                                                \
    return p;                                                                                      \
  }
#else
#define V(n, p)                                                                                    \
  if (strncasecmp(name, n, 7) == 0) {                                                              \
    return p;                                                                                      \
  }
#endif
  // Only the following primes are supported based on security concerns about the smaller prime
  // groups (https://www.rfc-editor.org/rfc/rfc8247#section-2.4).
  V("modp14", BN_get_rfc3526_prime_2048);
  V("modp15", BN_get_rfc3526_prime_3072);
  V("modp16", BN_get_rfc3526_prime_4096);
  V("modp17", BN_get_rfc3526_prime_6144);
  V("modp18", BN_get_rfc3526_prime_8192);
#undef V

  return nullptr;
}

kj::Own<DH> initDhGroup(kj::StringPtr name) {
  auto group = findDiffieHellmanGroup(name.begin());
  JSG_REQUIRE(group != nullptr, Error,
      "Failed to init DiffieHellmanGroup: invalid group. Only "
      "groups {modp14, modp15, modp16, modp17, modp18} are supported.");
  auto groupKey = group(nullptr);
  KJ_ASSERT(groupKey != nullptr);

  const int kStandardizedGenerator = 2;
  auto dh = OSSL_NEW(DH);

  // Note: We're deliberately not using kj::Own/OSSL_NEW() here as DH_set0_pqg() takes ownership
  // of the key, so there is no need to free it if the operation succeeds.
  UniqueBignum bn_g(BN_new(), &BN_clear_free);
  if (!BN_set_word(bn_g.get(), kStandardizedGenerator) ||
      !DH_set0_pqg(dh, groupKey, nullptr, bn_g.get())) {
    JSG_FAIL_REQUIRE(Error, "DiffieHellmanGroup init failed: could not set keys");
  }
  bn_g.release();
  return kj::mv(dh);
}

kj::Own<DH> initDh(kj::OneOf<kj::Array<kj::byte>, int>& sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int>& generator) {
  KJ_SWITCH_ONEOF(sizeOrKey) {
    KJ_CASE_ONEOF(size, int) {
      KJ_SWITCH_ONEOF(generator) {
        KJ_CASE_ONEOF(gen, int) {
          // Generating a DH key with a reasonable size can be expensive.
          // We will only allow it if there is an active IoContext so that
          // we can enforce a timeout associate with the limit enforcer.
          JSG_REQUIRE(IoContext::hasCurrent(), Error,
              "DiffieHellman key generation requires an active request");

          struct Status {
            IoContext& context;
            kj::Maybe<EventOutcome> status;
          } status{.context = IoContext::current()};

          auto dh = OSSL_NEW(DH);
          BN_GENCB cb;
          cb.arg = &status;
          // This callback is called many times during the key generation process.
          // We use it because key generation is expensive and may run over the
          // CPU limits for the request. As this method can itself contribute to
          // running over the CPU limit, it is important to do as little as possible.
          cb.callback = [](int a, int b, BN_GENCB* cb) -> int {
            Status& status = *static_cast<Status*>(cb->arg);
            KJ_IF_SOME(outcome, status.context.getLimitEnforcer().getLimitsExceeded()) {
              status.status = outcome;
              return 0;
            }
            return 1;
          };
          // Operations on an "egregiously large" prime will throw with recent boringssl.
          // TODO(soon): Convert this and the other invalid parameter warning to user errors if
          // possible.
          if (size > OPENSSL_DH_MAX_MODULUS_BITS) {
            KJ_LOG(WARNING, "DiffieHellman init: requested prime size too large");
          }
          if (!DH_generate_parameters_ex(dh.get(), size, gen, &cb)) {
            KJ_IF_SOME(outcome, status.status) {
              if (outcome == EventOutcome::EXCEEDED_CPU) {
                JSG_FAIL_REQUIRE(
                    Error, "DiffieHellman init failed: key generation exceeded CPU limit");
              } else if (outcome == EventOutcome::EXCEEDED_MEMORY) {
                JSG_FAIL_REQUIRE(
                    Error, "DiffieHellman init failed: key generation exceeded memory limit");
              }
            }
            JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: could not generate parameters");
          }
          // Boringssl throws on DH with g >= p or g | 2 since g can't be an element of p's
          // multiplicative group in that case.
          if (!BN_is_odd(DH_get0_p(dh)) || BN_ucmp(DH_get0_g(dh), DH_get0_p(dh)) >= 0) {
            KJ_LOG(WARNING, "DiffieHellman init: Invalid generated DH prime");
          }
          return kj::mv(dh);
        }
        KJ_CASE_ONEOF(gen, kj::Array<kj::byte>) {
          // Node.js does not support generating Diffie-Hellman keys from an int prime
          // and byte-array generator. This could change in the future.
          JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: invalid parameters");
        }
      }
    }
    KJ_CASE_ONEOF(key, kj::Array<kj::byte>) {
      JSG_REQUIRE(
          key.size() <= INT32_MAX, RangeError, "DiffieHellman init failed: key is too large");
      JSG_REQUIRE(key.size() > 0, Error, "DiffieHellman init failed: invalid key");
      // Operations on an "egregiously large" prime will throw with boringssl.
      if (key.size() > OPENSSL_DH_MAX_MODULUS_BITS / CHAR_BIT) {
        KJ_LOG(WARNING, "DiffieHellman init: prime too large");
      }
      auto dh = OSSL_NEW(DH);

      // We use a std::unique_ptr here instead of a kj::Own because DH_set0_pqg takes ownership
      // and we need to be able to release ownership if the operation succeeds but want the
      // BIGNUMs to be appropriately freed if the operations fail.
      using UniqueBignum = std::unique_ptr<BIGNUM, void (*)(BIGNUM*)>;
      UniqueBignum bn_g(nullptr, &BN_clear_free);

      KJ_SWITCH_ONEOF(generator) {
        KJ_CASE_ONEOF(gen, int) {
          JSG_REQUIRE(gen >= 2, RangeError, "DiffieHellman init failed: generator too small");
          bn_g.reset(BN_new());
          if (!BN_set_word(bn_g.get(), gen)) {
            JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: could not set keys");
          }
        }
        KJ_CASE_ONEOF(gen, kj::Array<kj::byte>) {
          JSG_REQUIRE(gen.size() <= INT32_MAX, RangeError,
              "DiffieHellman init failed: generator is too large");
          JSG_REQUIRE(gen.size() > 0, Error, "DiffieHellman init failed: invalid generator");

          bn_g.reset(toBignumUnowned(gen));
          if (BN_is_zero(bn_g.get()) || BN_is_one(bn_g.get())) {
            JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: invalid generator");
          }
        }
      }
      UniqueBignum bn_p(toBignumUnowned(key), &BN_clear_free);
      JSG_REQUIRE(bn_p != nullptr, Error,
          "DiffieHellman init failed: could not convert key representation");
      // Boringssl throws on DH with g >= p or g | 2 since g can't be an element of p's
      // multiplicative group in that case.
      if (!BN_is_odd(bn_p.get()) || BN_ucmp(bn_g.get(), bn_p.get()) >= 0) {
        KJ_LOG(WARNING, "DiffieHellman init: Invalid DH prime generated");
      }
      JSG_REQUIRE(DH_set0_pqg(dh, bn_p.get(), nullptr, bn_g.get()), Error,
          "DiffieHellman init failed: could not set keys");
      bn_g.release();
      bn_p.release();
      return kj::mv(dh);
    }
  }
  KJ_UNREACHABLE;
}

void zeroPadDiffieHellmanSecret(size_t remainder_size, unsigned char* data, size_t prime_size) {
  // DH_size returns number of bytes in a prime number.
  // DH_compute_key returns number of bytes in a remainder of exponent, which
  // may have less bytes than a prime number. Therefore add 0-padding to the
  // allocated buffer.
  if (remainder_size != prime_size) {
    KJ_ASSERT(remainder_size < prime_size);
    const size_t padding = prime_size - remainder_size;
    memmove(data + padding, data, remainder_size);
    kj::arrayPtr(data, padding).fill(0);
  }
}
}  // namespace

DiffieHellman::DiffieHellman(kj::StringPtr group): dh(initDhGroup(group)) {}

DiffieHellman::DiffieHellman(
    kj::OneOf<kj::Array<kj::byte>, int>& sizeOrKey, kj::OneOf<kj::Array<kj::byte>, int>& generator)
    : dh(initDh(sizeOrKey, generator)) {}

kj::Maybe<int> DiffieHellman::check() {
  ClearErrorOnReturn clearErrorOnReturn;
  int codes;
  if (!DH_check(dh.get(), &codes)) {
    return kj::none;
  }
  return codes;
}

void DiffieHellman::setPrivateKey(kj::ArrayPtr<kj::byte> key) {
  OSSLCALL(DH_set0_key(dh, nullptr, toBignumUnowned(key)));
}

void DiffieHellman::setPublicKey(kj::ArrayPtr<kj::byte> key) {
  OSSLCALL(DH_set0_key(dh, toBignumUnowned(key), nullptr));
}

kj::Array<kj::byte> DiffieHellman::getPublicKey() {
  const BIGNUM* pub_key = DH_get0_pub_key(dh);
  return JSG_REQUIRE_NONNULL(
      bignumToArrayPadded(*pub_key), Error, "Error while retrieving DiffieHellman public key");
}

kj::Array<kj::byte> DiffieHellman::getPrivateKey() {
  const BIGNUM* priv_key = DH_get0_priv_key(dh);
  return JSG_REQUIRE_NONNULL(
      bignumToArrayPadded(*priv_key), Error, "Error while retrieving DiffieHellman private key");
}

kj::Array<kj::byte> DiffieHellman::getGenerator() {
  const BIGNUM* g = DH_get0_g(dh);
  return JSG_REQUIRE_NONNULL(
      bignumToArrayPadded(*g), Error, "Error while retrieving DiffieHellman generator");
}

kj::Array<kj::byte> DiffieHellman::getPrime() {
  const BIGNUM* p = DH_get0_p(dh);
  return JSG_REQUIRE_NONNULL(
      bignumToArrayPadded(*p), Error, "Error while retrieving DiffieHellman prime");
}

kj::Array<kj::byte> DiffieHellman::computeSecret(kj::ArrayPtr<kj::byte> key) {
  JSG_REQUIRE(key.size() <= INT32_MAX, RangeError,
      "DiffieHellman computeSecret() failed: key is too large");
  JSG_REQUIRE(key.size() > 0, Error, "DiffieHellman computeSecret() failed: invalid key");

  ClearErrorOnReturn clear_error_on_return;
  auto k = JSG_REQUIRE_NONNULL(
      toBignum(key), Error, "Error getting key while computing DiffieHellman secret");

  size_t prime_size = DH_size(dh);
  auto prime_enc = kj::heapArray<kj::byte>(prime_size);

  int size = DH_compute_key(prime_enc.begin(), k.get(), dh);
  if (size == -1) {
    // various error checking
    int checkResult;
    int checked = DH_check_pub_key(dh, k, &checkResult);

    if (checked && checkResult) {
      JSG_REQUIRE(!(checkResult & DH_CHECK_PUBKEY_TOO_SMALL), RangeError,
          "DiffieHellman computeSecret() failed: Supplied key is too small");
      JSG_REQUIRE(!(checkResult & DH_CHECK_PUBKEY_TOO_LARGE), RangeError,
          "DiffieHellman computeSecret() failed: Supplied key is too large");
    }
    JSG_FAIL_REQUIRE(Error, "Invalid Key");
  }

  KJ_ASSERT(size >= 0);
  zeroPadDiffieHellmanSecret(size, prime_enc.begin(), prime_size);
  return prime_enc;
}

kj::Array<kj::byte> DiffieHellman::generateKeys() {
  ClearErrorOnReturn clear_error_on_return;
  OSSLCALL(DH_generate_key(dh));
  const BIGNUM* pub_key = DH_get0_pub_key(dh);
  return JSG_REQUIRE_NONNULL(
      bignumToArrayPadded(*pub_key), Error, "Error while generating DiffieHellman keys");
}

}  // namespace workerd::api
