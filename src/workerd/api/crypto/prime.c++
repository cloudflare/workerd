#include "prime.h"

#include "impl.h"

#include <workerd/jsg/jsg.h>

#include <ncrypto.h>

namespace workerd::api {

jsg::BufferSource randomPrime(jsg::Lock& js,
    uint32_t size,
    bool safe,
    kj::Maybe<kj::ArrayPtr<kj::byte>> add_buf,
    kj::Maybe<kj::ArrayPtr<kj::byte>> rem_buf) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  // Use mapping to have kj::Own work with optional buffer
  static const auto toBignum =
      [](kj::Maybe<kj::ArrayPtr<kj::byte>>& maybeBignum) -> ncrypto::BignumPointer {
    KJ_IF_SOME(a, maybeBignum) {
      if (auto bn = ncrypto::BignumPointer(a.begin(), a.size())) {
        return bn;
      }
      JSG_FAIL_REQUIRE(
          RangeError, "Error importing add parameter", internalDescribeOpensslErrors());
    };
    return {};
  };

  auto add = toBignum(add_buf);
  auto rem = toBignum(rem_buf);
  // The JS interface already ensures that the (positive) size fits into an int.
  int bits = static_cast<int>(size);

  if (add) {
    // Currently, we only allow certain values for add and rem due to a bug in
    // the BN_generate_prime_ex that allows invalid values to enter an infinite
    // loop. This diverges from the Node.js implementation a bit but that's OK.
    // The key use case for this function is generating DH parameters and those
    // have pretty specific values for various generators anyway.
    // Specifically, we limit the values of add and rem to match the specific
    // pairings: add: 12, rem 11, add 24, rem 23, and add 60, rem 59.
    // If users complain about this, we can always remove this check and try
    // to get the infinite loop bug fixed.

    auto addCheck = ncrypto::BignumPointer::New();
    auto remCheck = ncrypto::BignumPointer::New();
    const auto checkAddRem = [&](auto anum, auto bnum) {
      addCheck.setWord(anum);
      remCheck.setWord(bnum);
      return BN_cmp(add.get(), addCheck.get()) == 0 && BN_cmp(rem.get(), remCheck.get()) == 0;
    };

    JSG_REQUIRE(rem && (checkAddRem(12, 11) || checkAddRem(24, 23) || checkAddRem(60, 59)),
        RangeError, "Invalid values for add and rem");

    // This would definitely lead to an infinite loop if allowed since
    // OpenSSL does not check this condition.
    JSG_REQUIRE(add > rem, RangeError, "options.rem must be smaller than options.add");

    // If we allowed this, the best case would be returning a static prime
    // that wasn't generated randomly. The worst case would be an infinite
    // loop within OpenSSL, blocking the main thread or one of the threads
    // in the thread pool.
    JSG_REQUIRE(add.bitLength() <= bits, RangeError,
        "options.add must not be bigger than size of the requested prime");
  }

  // Generating random primes uses the PRNG internally.
  // Make sure the CSPRNG is properly seeded.
  JSG_REQUIRE(
      workerd::api::CSPRNG(nullptr), Error, "Error while generating prime (bad random state)");

  if (auto prime = ncrypto::BignumPointer::NewPrime({
        .bits = bits,
        .safe = safe,
        .add = kj::mv(add),
        .rem = kj::mv(rem),
      })) {
    return JSG_REQUIRE_NONNULL(
        bignumToArrayPadded(js, *prime.get()), Error, "Error while generating prime");
  }

  JSG_FAIL_REQUIRE(Error, "Error while generating prime");
}

bool checkPrime(kj::ArrayPtr<kj::byte> bufferView, uint32_t num_checks) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;
  static constexpr int32_t kMaxChecks = kj::maxValue;
  // Strictly upper bound the number of checks. If this proves to be too expensive
  // then we may need to consider lowering this limit further.
  JSG_REQUIRE(num_checks <= kMaxChecks, RangeError, "Invalid number of checks");

  auto candidate = ncrypto::BignumPointer(bufferView.begin(), bufferView.size());
  JSG_REQUIRE(candidate, Error, "Error while checking prime");
  return candidate.isPrime(num_checks);
}

}  // namespace workerd::api
