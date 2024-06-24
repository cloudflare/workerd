// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "crypto.h"
#include "crypto-util.h"
#include <v8.h>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto/impl.h>

namespace workerd::api::node {
  kj::Array<kj::byte> CryptoImpl::randomPrime(uint32_t size, bool safe,
      jsg::Optional<kj::Array<kj::byte>> add_buf, jsg::Optional<kj::Array<kj::byte>> rem_buf) {
  // Use mapping to have kj::Own work with optional buffer
  const auto maybeOwnBignum = [](jsg::Optional<kj::Array<kj::byte>>& maybeBignum) {
    return maybeBignum.map([](kj::Array<kj::byte>& a) {
      return OSSLCALL_OWN(BIGNUM, BN_bin2bn(a.begin(), a.size(), nullptr),
          RangeError, "Error importing add parameter", internalDescribeOpensslErrors());
      });
  };

  BIGNUM* add = nullptr;
  auto _add = maybeOwnBignum(add_buf);
  KJ_IF_SOME(a, _add) {
      add = a.get();
  }

  BIGNUM* rem = nullptr;
  auto _rem = maybeOwnBignum(rem_buf);
  KJ_IF_SOME(r, _rem) {
      rem = r.get();
  }

  if (add != nullptr) {
    // Currently, we only allow certain values for add and rem due to a bug in
    // the BN_generate_prime_ex that allows invalid values to enter an infinite
    // loop. This diverges fromt the Node.js implementation a bit but that's ok.
    // The key use case for this function is generating DH parameters and those
    // have pretty specific values for various generators anyway.
    // Specifically, we limit the values of add and rem to match the specific
    // pairings: add: 12, rem 11, add 24, rem 23, and add 60, rem 59.
    // If users complain about this, we can always remove this check and try
    // to get the infinite loop bug fixed.

    auto addCheck = OSSL_NEW(BIGNUM);
    auto remCheck = OSSL_NEW(BIGNUM);
    const auto checkAddRem = [&](auto anum, auto bnum) {
      BN_set_word(addCheck.get(), anum);
      BN_set_word(remCheck.get(), bnum);
      return BN_cmp(add, addCheck.get()) == 0 && BN_cmp(rem, remCheck.get()) == 0;
    };

    JSG_REQUIRE(rem != nullptr && (checkAddRem(12, 11) || checkAddRem(24, 23) ||
                checkAddRem(60, 59)), RangeError, "Invalid values for add and rem");
  }

  // The JS interface already ensures that the (positive) size fits into an int.
  int bits = static_cast<int>(size);

  if (add) {
      // If we allowed this, the best case would be returning a static prime
      // that wasn't generated randomly. The worst case would be an infinite
      // loop within OpenSSL, blocking the main thread or one of the threads
      // in the thread pool.
    JSG_REQUIRE(BN_num_bits(add) <= bits, RangeError,
        "options.add must not be bigger than size of the requested prime");

    if (rem) {
      // This would definitely lead to an infinite loop if allowed since
      // OpenSSL does not check this condition.
      JSG_REQUIRE(BN_cmp(add, rem) == 1, RangeError,
          "options.rem must be smaller than options.add");
    }
  }

  // BN_generate_prime_ex() calls RAND_bytes_ex() internally.
  // Make sure the CSPRNG is properly seeded.
  JSG_REQUIRE(workerd::api::node::CryptoUtil::CSPRNG(nullptr, 0).is_ok(), Error,
      "Error while generating prime (bad random state)");

  auto prime = OSSL_NEW(BIGNUM);

  int ret = BN_generate_prime_ex(
          prime.get(),
          bits,
          safe ? 1 : 0,
          add,
          rem,
          nullptr);
  JSG_REQUIRE(ret == 1, Error, "Error while generating prime");

  size_t prime_size = BN_num_bytes(prime.get());
  auto prime_enc = kj::heapArray<kj::byte>(prime_size);
  int next = BN_bn2binpad(
      prime.get(),
      prime_enc.begin(),
      prime_size);
  JSG_REQUIRE(next == (int)prime_size, Error, "Error while generating prime");
  return prime_enc;
}

bool CryptoImpl::checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks) {
  auto candidate = OSSLCALL_OWN(BIGNUM, BN_bin2bn(bufferView.begin(), bufferView.size(), nullptr),
    Error, "Error while checking prime");
  auto ctx = OSSL_NEW(BN_CTX);
  int ret = BN_is_prime_ex(candidate.get(), num_checks, ctx.get(), nullptr);
  JSG_REQUIRE(ret >= 0, Error, "Error while checking prime");
  return ret > 0;
}

}  // namespace workerd::api::node
