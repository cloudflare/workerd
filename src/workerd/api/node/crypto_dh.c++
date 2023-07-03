// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <workerd/api/crypto-impl.h>
#include <workerd/jsg/jsg.h>

// Import DH primes if needed.
#if WORKERD_BSSL_NEED_DH_PRIMES
#include "dh_primes.h"
#endif // WORKERD_BSSL_NEED_DH_PRIMES

#if !_WIN32
#include <strings.h>
#endif

namespace workerd::api::node {

namespace {
// Returns a function that can be used to create an instance of a standardized
// Diffie-Hellman group.
BIGNUM* (*FindDiffieHellmanGroup(const char* name))(BIGNUM*) {
#if _WIN32
#define V(n, p) if (_strnicmp(name, n, 7) == 0) {return p;}
#else
#define V(n, p) if (strncasecmp(name, n, 7) == 0) {return p;}
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
} // namespace

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanGroupHandle(kj::String name) {
  return jsg::alloc<DiffieHellmanHandle>(name);
}

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanHandle::constructor(
    jsg::Lock &js, kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> generator) {
  return jsg::alloc<DiffieHellmanHandle>(sizeOrKey, generator);
}

bool CryptoImpl::DiffieHellmanHandle::VerifyContext() {
  int codes;
  if (!DH_check(dh, &codes))
    return false;
  verifyError = codes;
  return true;
}

CryptoImpl::DiffieHellmanHandle::DiffieHellmanHandle(
    kj::OneOf<kj::Array<kj::byte>, int> &sizeOrKey, kj::OneOf<kj::Array<kj::byte>,
    int> &generator) : verifyError(0) {
  JSG_REQUIRE(Init(sizeOrKey, generator), Error, "DiffieHellman init failed");
}

CryptoImpl::DiffieHellmanHandle::DiffieHellmanHandle(kj::String& name) : verifyError(0) {
  JSG_REQUIRE(InitGroup(name), Error, "DiffieHellman init failed");
}

bool CryptoImpl::DiffieHellmanHandle::InitGroup(kj::String& name) {
  auto group = FindDiffieHellmanGroup(name.begin());
  JSG_REQUIRE(group != nullptr, Error, "Failed to init DiffieHellmanGroup: invalid group. At "
              "this time, only group 'modp5' is supported.");
  auto groupKey = group(nullptr);
  KJ_ASSERT(groupKey != nullptr);

  const int kStandardizedGenerator = 2;
  dh = OSSL_NEW(DH);

  // Note: We're deliberately not using kj::Own/OSSL_NEW() here as  DH_set0_pqg() takes ownership
  // of the key, so there is no need to free it if the operation succeeds.
  BIGNUM* bn_g = BN_new();
  if (!BN_set_word(bn_g, kStandardizedGenerator) || !DH_set0_pqg(dh, groupKey, nullptr, bn_g)) {
    BN_free(bn_g);
    JSG_FAIL_REQUIRE(Error, "DiffieHellmanGroup init failed: could not set keys");
  }
  return VerifyContext();
}

bool CryptoImpl::DiffieHellmanHandle::Init(
    kj::OneOf<kj::Array<kj::byte>, int> &sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> &generator) {
  KJ_SWITCH_ONEOF(sizeOrKey) {
    KJ_CASE_ONEOF(size, int) {
      KJ_SWITCH_ONEOF(generator) {
        KJ_CASE_ONEOF(gen, int) {
          // DH key generation is not supported at this time.
          JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: key generation is not supported, "
              "please provide a prime or use DiffieHellmanGroup instead.");
          return VerifyContext();
        }
        KJ_CASE_ONEOF(gen, kj::Array<kj::byte>) {
          // Node returns an error in this configuration, not sure why
          JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: invalid parameters");
        }
      }
    }
    KJ_CASE_ONEOF(key, kj::Array<kj::byte>) {
      JSG_REQUIRE(key.size() <= INT32_MAX, RangeError, "DiffieHellman init failed: key "
                  "is too large");
      JSG_REQUIRE(key.size() > 0, Error, "DiffieHellman init failed: invalid key");
      dh = OSSL_NEW(DH);
      BIGNUM* bn_g;

      KJ_SWITCH_ONEOF(generator) {
        KJ_CASE_ONEOF(gen, int) {
          JSG_REQUIRE(gen >= 2, RangeError, "DiffieHellman init failed: generator too small");
          bn_g = BN_new();
          if (!BN_set_word(bn_g, gen)) {
            BN_free(bn_g);
            JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: could not set keys");
          }
        }
        KJ_CASE_ONEOF(gen, kj::Array<kj::byte>) {
          JSG_REQUIRE(gen.size() <= INT32_MAX, RangeError,
                      "DiffieHellman init failed: generator is too large");
          JSG_REQUIRE(gen.size() > 0, Error, "DiffieHellman init failed: invalid generator");

          bn_g = BN_bin2bn(gen.begin(), gen.size(), nullptr);
          if (BN_is_zero(bn_g) || BN_is_one(bn_g)) {
            BN_free(bn_g);
            JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: invalid generator");
          }
        }
      }
      BIGNUM* bn_p = BN_bin2bn(key.begin(), key.size(), nullptr);
      if (!bn_p) {
        BN_free(bn_g);
        JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: could not convert key representation");
      }
      if (!DH_set0_pqg(dh, bn_p, nullptr, bn_g)) {
        BN_free(bn_p);
        BN_free(bn_g);
        JSG_FAIL_REQUIRE(Error, "DiffieHellman init failed: could not set keys");
      }
      return VerifyContext();
    }
  }

  KJ_UNREACHABLE;
}

void CryptoImpl::DiffieHellmanHandle::setPrivateKey(kj::Array<kj::byte> key) {
  BIGNUM* k = BN_bin2bn(key.begin(), key.size(), nullptr);
  OSSLCALL(DH_set0_key(dh, nullptr, k));
}

void CryptoImpl::DiffieHellmanHandle::setPublicKey(kj::Array<kj::byte> key) {
  BIGNUM* k = BN_bin2bn(key.begin(), key.size(), nullptr);
  OSSLCALL(DH_set0_key(dh, k, nullptr));
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPublicKey() {
  const BIGNUM *pub_key;
  DH_get0_key(dh, &pub_key, nullptr);

  size_t key_size = BN_num_bytes(pub_key);
  auto key_enc = kj::heapArray<kj::byte>(key_size);
  int next = BN_bn2binpad(pub_key, key_enc.begin(), key_size);
  JSG_REQUIRE(next == (int)key_size, Error, "Error while retrieving DiffieHellman public key");
  return key_enc;
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPrivateKey() {
  const BIGNUM *priv_key;
  DH_get0_key(dh, nullptr, &priv_key);

  size_t key_size = BN_num_bytes(priv_key);
  auto key_enc = kj::heapArray<kj::byte>(key_size);
  int next = BN_bn2binpad(priv_key, key_enc.begin(), key_size);
  JSG_REQUIRE(next == (int)key_size, Error, "Error while retrieving DiffieHellman private key");
  return key_enc;
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getGenerator() {
  const BIGNUM* g;
  DH_get0_pqg(dh, nullptr, nullptr, &g);

  size_t gen_size = BN_num_bytes(g);
  auto gen_enc = kj::heapArray<kj::byte>(gen_size);
  int next = BN_bn2binpad(g, gen_enc.begin(), gen_size);
  JSG_REQUIRE(next == (int)gen_size, Error, "Error while retrieving DiffieHellman generator");
  return gen_enc;
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::getPrime() {
  const BIGNUM* p;
  DH_get0_pqg(dh, &p, nullptr, nullptr);

  size_t prime_size = BN_num_bytes(p);
  auto prime_enc = kj::heapArray<kj::byte>(prime_size);
  int next = BN_bn2binpad(p, prime_enc.begin(), prime_size);
  JSG_REQUIRE(next == (int)prime_size, Error, "Error while retrieving DiffieHellman prime");
  return prime_enc;
}

namespace {
void ZeroPadDiffieHellmanSecret(size_t remainder_size,
                                unsigned char* data,
                                size_t prime_size) {
  // DH_size returns number of bytes in a prime number.
  // DH_compute_key returns number of bytes in a remainder of exponent, which
  // may have less bytes than a prime number. Therefore add 0-padding to the
  // allocated buffer.
  if (remainder_size != prime_size) {
    KJ_ASSERT(remainder_size < prime_size);
    const size_t padding = prime_size - remainder_size;
    memmove(data + padding, data, remainder_size);
    memset(data, 0, padding);
  }
}
} // namespace

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::computeSecret(kj::Array<kj::byte> key) {
  JSG_REQUIRE(key.size() <= INT32_MAX, RangeError,
              "DiffieHellman computeSecret() failed: key is too large");
  JSG_REQUIRE(key.size() > 0, Error, "DiffieHellman computeSecret() failed: invalid key");

  ClearErrorOnReturn clear_error_on_return;
  auto k = OSSLCALL_OWN(BIGNUM, BN_bin2bn(key.begin(), key.size(), nullptr), Error,
                        "Error getting key while computing DiffieHellman secret");
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
  ZeroPadDiffieHellmanSecret(size, prime_enc.begin(), prime_size);
  return prime_enc;
}

kj::Array<kj::byte> CryptoImpl::DiffieHellmanHandle::generateKeys() {
  OSSLCALL(DH_generate_key(dh));
  const BIGNUM* pub_key;
  DH_get0_key(dh, &pub_key, nullptr);

  const int size = BN_num_bytes(pub_key);
  auto prime_enc = kj::heapArray<kj::byte>(size);

  KJ_ASSERT(size > 0);
  JSG_REQUIRE(size == BN_bn2binpad(pub_key, prime_enc.begin(), size), Error,
              "failed to convert DiffieHellman key representation");

  return prime_enc;
}

int CryptoImpl::DiffieHellmanHandle::getVerifyError() { return verifyError; }

} // namespace workerd::api::node
