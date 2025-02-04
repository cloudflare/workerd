#include "spkac.h"

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

#include <ncrypto.h>

namespace workerd::api {

bool verifySpkac(kj::ArrayPtr<const kj::byte> input) {
  // So, this is fun. SPKAC uses MD5 as the digest algorithm. This is a problem because
  // using MD5 for signature verification is not allowed in FIPS mode, which means that
  // although we have a working implementation here, the result of this call is always
  // going to be false even if the input signature is correct. So this is a bit of a dead
  // end that isn't going to be super useful. Fortunately tho the exportPublicKey and
  // exportChallenge functions both work correctly and are useful. Unfortunately, this
  // likely means users would need to implement their own verification, which sucks.
  //
  // Alternatively we could choose to implement our own version of the validation that
  // bypasses BoringSSL's FIPS configuration. For now tho, this does end up matching
  // Node.js' behavior when FIPS is enabled so I guess that's something.
  if (IoContext::hasCurrent()) {
    IoContext::current().logWarningOnce(
        "The verifySpkac function is currently of limited value in workers because "
        "the SPKAC signature verification uses MD5, which is not supported in FIPS mode. "
        "All workers run in FIPS mode. Accordingly, this method will currently always "
        "return false even if the SPKAC signature is valid. This is a known limitation.");
  }

  ncrypto::Buffer<const char> buf{
    .data = reinterpret_cast<const char*>(input.begin()),
    .len = input.size(),
  };
  return ncrypto::VerifySpkac(buf);
}

kj::Maybe<jsg::BufferSource> exportPublicKey(jsg::Lock& js, kj::ArrayPtr<const kj::byte> input) {
  ncrypto::Buffer<const char> buf{
    .data = reinterpret_cast<const char*>(input.begin()),
    .len = input.size(),
  };
  if (auto bio = ncrypto::ExportPublicKey(buf)) {
    BUF_MEM* bptr = bio;
    auto buf = jsg::BackingStore::alloc(js, bptr->length);
    auto aptr = kj::arrayPtr(bptr->data, bptr->length);
    buf.asArrayPtr<char>().copyFrom(aptr);
    return jsg::BufferSource(js, kj::mv(buf));
  }
  return kj::none;
}

kj::Maybe<jsg::BufferSource> exportChallenge(jsg::Lock& js, kj::ArrayPtr<const kj::byte> input) {
  ncrypto::Buffer<const char> buf{
    .data = reinterpret_cast<const char*>(input.begin()),
    .len = input.size(),
  };
  if (auto dp = ncrypto::ExportChallenge(buf)) {
    auto dest = jsg::BackingStore::alloc(js, dp.size());
    auto src = kj::arrayPtr(dp.get<kj::byte>(), dp.size());
    dest.asArrayPtr().copyFrom(src);
    return jsg::BufferSource(js, kj::mv(dest));
  }
  return kj::none;
}
}  // namespace workerd::api
