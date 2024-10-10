#include "spkac.h"

#include "impl.h"

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/strings.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

namespace workerd::api {
namespace {
kj::Array<kj::byte> toArray(BIO* bio) {
  BUF_MEM* bptr;
  BIO_get_mem_ptr(bio, &bptr);
  auto buf = kj::heapArray<char>(bptr->length);
  auto aptr = kj::arrayPtr(bptr->data, bptr->length);
  buf.asPtr().copyFrom(aptr);
  return buf.releaseAsBytes();
}

kj::Maybe<kj::Own<NETSCAPE_SPKI>> tryGetSpki(kj::ArrayPtr<const kj::byte> input) {
  static constexpr int32_t kMaxLength = kj::maxValue;
  JSG_REQUIRE(input.size() <= kMaxLength, RangeError, "spkac is too large");
  auto trimmed = trimTailingWhitespace(input.asChars());
  auto ptr = NETSCAPE_SPKI_b64_decode(trimmed.begin(), trimmed.size());
  if (!ptr) return kj::none;
  return kj::disposeWith<NETSCAPE_SPKI_free>(ptr);
}

kj::Maybe<kj::Own<EVP_PKEY>> tryOwnPkey(kj::Own<NETSCAPE_SPKI>& spki) {
  auto pkey = NETSCAPE_SPKI_get_pubkey(spki.get());
  if (!pkey) return kj::none;
  return kj::disposeWith<EVP_PKEY_free>(pkey);
}

kj::Maybe<kj::Own<BIO>> tryNewBio() {
  auto bioptr = BIO_new(BIO_s_mem());
  if (!bioptr) return kj::none;
  return kj::disposeWith<BIO_free_all>(bioptr);
}
}  // namespace

bool verifySpkac(kj::ArrayPtr<const kj::byte> input) {
  // So, this is fun. SPKAC uses MD5 as the digest algorithm. This is a problem because
  // using MD5 for signature verification is not allowed in FIPS mode, which means that
  // although we have a working implementation here, the result of this call is always
  // going to false even if the input signature is correct. So this is a bit of a dead
  // end that isn't going to be super useful. Fortunately but the exportPublicKey and
  // exportChallenge functions both work correctly and are useful. Unfortunately, this
  // likely means users would need to implement their own verification, which sucks.
  //
  // Alternatively we could choose to implement our own version of the validation that
  // bypasses BoringSSL's FIPS configuration. For now tho, this does end up matching
  // Node.js' behavior when FIPS is enabled so I guess that's something.
  ClearErrorOnReturn clearErrorOnReturn;
  if (IoContext::hasCurrent()) {
    IoContext::current().logWarningOnce(
        "The verifySpkac function is currently of limited value in workers because "
        "the SPKAC signature verification uses MD5, which is not supported in FIPS mode. "
        "All workers run in FIPS mode. Accordingly, this method will currently always "
        "return false even if the SPKAC signature is valid. This is a known limitation.");
  }
  KJ_IF_SOME(spki, tryGetSpki(input)) {
    KJ_IF_SOME(key, tryOwnPkey(spki)) {
      return NETSCAPE_SPKI_verify(spki.get(), key.get()) > 0;
    }
  }
  return false;
}

kj::Maybe<kj::Array<kj::byte>> exportPublicKey(kj::ArrayPtr<const kj::byte> input) {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(spki, tryGetSpki(input)) {
    KJ_IF_SOME(bio, tryNewBio()) {
      KJ_IF_SOME(key, tryOwnPkey(spki)) {
        if (PEM_write_bio_PUBKEY(bio.get(), key.get()) > 0) {
          return toArray(bio.get());
        }
      }
    }
  }
  return kj::none;
}

kj::Maybe<kj::Array<kj::byte>> exportChallenge(kj::ArrayPtr<const kj::byte> input) {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(spki, tryGetSpki(input)) {
    kj::byte* buf = nullptr;
    int buf_size = ASN1_STRING_to_UTF8(&buf, spki->spkac->challenge);
    if (buf_size < 0 || buf == nullptr) return kj::none;
    // Pay attention to how the buffer is freed below...
    return kj::arrayPtr(buf, buf_size).attach(kj::defer([buf]() { OPENSSL_free(buf); }));
  }
  return kj::none;
}
}  // namespace workerd::api
