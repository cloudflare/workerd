#include "prime.h"

#include <openssl/bn.h>
#include <workerd/api/crypto/impl.h>

namespace workerd::api {

kj::Array<kj::byte> randomPrime(uint32_t size,
    bool safe,
    kj::Maybe<kj::ArrayPtr<kj::byte>> add_buf,
    kj::Maybe<kj::ArrayPtr<kj::byte>> rem_buf) {
  ClearErrorOnReturn clearErrorOnReturn;

  // Use mapping to have kj::Own work with optional buffer
  const auto maybeOwnBignum = [](kj::Maybe<kj::ArrayPtr<kj::byte>>& maybeBignum) {
    return maybeBignum.map([](kj::ArrayPtr<kj::byte>& a) {
      return JSG_REQUIRE_NONNULL(toBignum(a), RangeError, "Error importing add parameter",
          internalDescribeOpensslErrors());
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

    JSG_REQUIRE(
        rem != nullptr && (checkAddRem(12, 11) || checkAddRem(24, 23) || checkAddRem(60, 59)),
        RangeError, "Invalid values for add and rem");
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
      JSG_REQUIRE(
          BN_cmp(add, rem) == 1, RangeError, "options.rem must be smaller than options.add");
    }
  }

  // BN_generate_prime_ex() calls RAND_bytes_ex() internally.
  // Make sure the CSPRNG is properly seeded.
  JSG_REQUIRE(
      workerd::api::CSPRNG(nullptr), Error, "Error while generating prime (bad random state)");

  auto prime = OSSL_NEW(BIGNUM);

  int ret = BN_generate_prime_ex(prime.get(), bits, safe ? 1 : 0, add, rem, nullptr);
  JSG_REQUIRE(ret == 1, Error, "Error while generating prime");

  return JSG_REQUIRE_NONNULL(bignumToArrayPadded(*prime), Error, "Error while generating prime");
}

bool checkPrime(kj::ArrayPtr<kj::byte> bufferView, uint32_t num_checks) {
  ClearErrorOnReturn clearErrorOnReturn;
  static constexpr int32_t kMaxChecks = kj::maxValue;
  // Strictly upper bound the number of checks. If this proves to be too expensive
  // then we may need to consider lowering this limit further.
  JSG_REQUIRE(num_checks <= kMaxChecks, RangeError, "Invalid number of checks");
  auto candidate = JSG_REQUIRE_NONNULL(toBignum(bufferView), Error, "Error while checking prime");
  auto ctx = OSSL_NEW(BN_CTX);
  int ret = BN_is_prime_ex(candidate.get(), num_checks, ctx.get(), nullptr);
  JSG_REQUIRE(ret >= 0, Error, "Error while checking prime");
  return ret > 0;
}

}  // namespace workerd::api
