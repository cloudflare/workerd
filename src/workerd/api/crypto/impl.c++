// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <workerd/api/util.h>
#include <workerd/jsg/memory.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <map>

namespace workerd::api {
namespace {
kj::String errorsToString(
    kj::Array<kj::OneOf<kj::StringPtr, OpensslUntranslatedError>> accumulatedErrors,
    kj::StringPtr defaultIfNoError) {
  if (accumulatedErrors.size() == 0) {
    return kj::str(defaultIfNoError);
  }

  if (accumulatedErrors.size() == 1) {
    kj::String heap;
    kj::StringPtr description;
    KJ_SWITCH_ONEOF(accumulatedErrors[0]) {
      KJ_CASE_ONEOF(e, kj::StringPtr) {
        description = e;
      }
      KJ_CASE_ONEOF(e, OpensslUntranslatedError) {
        heap = kj::str(e.library, " ", e.reasonName);
        description = heap;
      }
    }
    return kj::str(": ", description, ".");
  }

  return kj::str(": ",
      kj::strArray(
          KJ_MAP(e, accumulatedErrors) {
    KJ_SWITCH_ONEOF(accumulatedErrors[0]) {
      KJ_CASE_ONEOF(e, kj::StringPtr) {
        return e;
      }
      KJ_CASE_ONEOF(e, OpensslUntranslatedError) {
        return e.reasonName;
      }
    }
    KJ_UNREACHABLE;
  }, " "),
      ".");
}
}  // namespace

const SslArrayDisposer SslArrayDisposer::INSTANCE;

void SslArrayDisposer::disposeImpl(void* firstElement,
    size_t elementSize,
    size_t elementCount,
    size_t capacity,
    void (*destroyElement)(void*)) const {
  OPENSSL_free(firstElement);
}

// Call when an OpenSSL function returns an error code to convert that into an exception and
// throw it.
void throwOpensslError(const char* file, int line, kj::StringPtr code) {
  // Some error codes that we know are the application's fault are converted to app errors.
  // We only attempt to convert the most-recent error in the queue this way, because other errors
  // in the queue might have been accidentally left there by previous, unrelated operations.
  // Unfortunately BoringSSL's ERR_error_string() and friends produce unfriendly strings that
  // mostly just tell you the error constant name, which isn't what we want to throw at users.
  switch (ERR_GET_LIB(ERR_peek_last_error())) {
    // The error code defines overlap between the different BoringSSL libraries (for example, we
    // have EC_R_INVALID_ENCODING == RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY), so we must check the
    // library code.
    case ERR_LIB_EC:
      switch (ERR_GET_REASON(ERR_peek_last_error())) {
#define MAP_ERROR(CODE, TEXT)                                                                      \
  case CODE: {                                                                                     \
    ClearErrorOnReturn clearErrorOnReturn;                                                         \
    kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, file, line,                 \
        kj::str(JSG_EXCEPTION(DOMOperationError) ": ", TEXT)));                                    \
  }

        MAP_ERROR(EC_R_INVALID_ENCODING, "Invalid point encoding.")
        MAP_ERROR(EC_R_INVALID_COMPRESSED_POINT, "Invalid compressed point.")
        MAP_ERROR(EC_R_POINT_IS_NOT_ON_CURVE, "Point is not on curve.")
        default:
          break;
      };
      break;
    case ERR_LIB_RSA:
      switch (ERR_GET_REASON(ERR_peek_last_error())) {
        MAP_ERROR(RSA_R_DATA_LEN_NOT_EQUAL_TO_MOD_LEN, "Invalid RSA signature.");
#undef MAP_ERROR

        default:
          break;
      };
      break;
    default:
      // not an error code to be converted to app error, move on
      break;
  };

  // We don't recognize the error as one that is the app's fault, so assume it is an internal
  // error. Here we'll accept BoringSSL's ugly error strings as hopefully it's at least something
  // we can decipher.
  kj::Vector<kj::String> lines;
  while (unsigned long long error = ERR_get_error()) {
    char message[1024]{};
    ERR_error_string_n(error, message, sizeof(message));
    lines.add(kj::heapString(message));
  }
  kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, file, line,
      kj::str("OpenSSL call failed: ", code, "; ",
          lines.size() == 0 ? "but ERR_get_error() returned 0"_kj : kj::strArray(lines, "; "))));
}

kj::Vector<kj::OneOf<kj::StringPtr, OpensslUntranslatedError>> consumeAllOpensslErrors() {
  kj::Vector<kj::OneOf<kj::StringPtr, OpensslUntranslatedError>> accumulatedErrors;

  while (auto error = ERR_get_error()) {
    accumulatedErrors.add([error]() -> kj::OneOf<kj::StringPtr, OpensslUntranslatedError> {
      switch (ERR_GET_LIB(error)) {
        case ERR_LIB_RSA:
          switch (ERR_GET_REASON(error)) {
            case RSA_R_DATA_LEN_NOT_EQUAL_TO_MOD_LEN:
              return "Invalid RSA signature."_kj;
          }
          break;
        case ERR_LIB_EC:
          switch (ERR_GET_REASON(error)) {
            case EC_R_INVALID_ENCODING:
              return "Invalid point encoding."_kj;
            case EC_R_INVALID_COMPRESSED_POINT:
              return "Invalid compressed point."_kj;
            case EC_R_POINT_IS_NOT_ON_CURVE:
              return "Point is not on curve."_kj;
            case EC_R_UNKNOWN_GROUP:
              return "Unsupported elliptic curve group."_kj;
          }
          break;
      }

      return OpensslUntranslatedError{
        .library = ERR_lib_error_string(error),
        .reasonName = ERR_reason_error_string(error),
      };
    }());
  }

  return accumulatedErrors;
}

kj::String tryDescribeOpensslErrors(kj::StringPtr defaultIfNoError) {
  if (defaultIfNoError.size() == 0) {
    defaultIfNoError = "."_kj;
  }

  auto accumulatedErrors = consumeAllOpensslErrors();

  // For now we only allow errors we explicitly map to friendly strings to be displayed to end
  // users. #if 1 is convenient as it makes it easy to #if 0 to see the error codes printed when
  // debugging issues.
#if 1
  auto removeBegin = std::remove_if(accumulatedErrors.begin(), accumulatedErrors.end(),
      [](const auto& error) { return error.template is<OpensslUntranslatedError>(); });

  accumulatedErrors.resize(removeBegin - accumulatedErrors.begin());
#endif

  return errorsToString(accumulatedErrors.releaseAsArray(), defaultIfNoError);
}

kj::String internalDescribeOpensslErrors() {
  return errorsToString(consumeAllOpensslErrors().releaseAsArray(), "."_kj);
}

std::pair<kj::StringPtr, const EVP_MD*> lookupDigestAlgorithm(kj::StringPtr algorithm) {
  static const std::map<kj::StringPtr, const EVP_MD*, CiLess> registeredAlgorithms{
    {"SHA-1", EVP_sha1()},
    {"SHA-256", EVP_sha256()},
    {"SHA-384", EVP_sha384()},
    {"SHA-512", EVP_sha512()},

    // MD5 is not supported by WebCrypto, presumably because the designers didn't want to
    // support broken crypto. However, the reality is that people still use MD5 for things, and if
    // we don't give them a native implementation, they're going to use a pure-JS implementation,
    // leaving everyone worse-off.
    {"MD5", EVP_md5()},
  };

  auto algIter = registeredAlgorithms.find(algorithm);
  JSG_REQUIRE(algIter != registeredAlgorithms.end(), DOMNotSupportedError,
      "Unrecognized or unimplemented digest algorithm requested.");
  return *algIter;
}

kj::EncodingResult<kj::Array<kj::byte>> decodeBase64Url(kj::String text) {
  // TODO(cleanup): Make a non-mutating version of this and put in kj-encoding. Or add a
  //   "bool urlEncoded = false" parameter to kj::decodeBase64()?

  std::replace(text.begin(), text.end(), '-', '+');
  std::replace(text.begin(), text.end(), '_', '/');
  return kj::decodeBase64(text);
}

bool CryptoKey::Impl::equals(const kj::Array<kj::byte>& other) const {
  KJ_FAIL_REQUIRE("Unable to compare raw key material for this key");
}

bool CryptoKey::Impl::equals(const jsg::BufferSource& other) const {
  KJ_FAIL_REQUIRE("Unable to compare raw key material for this key");
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::from(kj::Own<EVP_PKEY> key) {
  switch (EVP_PKEY_id(key.get())) {
    case EVP_PKEY_RSA:
      return fromRsaKey(kj::mv(key));
    case EVP_PKEY_EC:
      return fromEcKey(kj::mv(key));
    case EVP_PKEY_ED25519:
      return fromEd25519Key(kj::mv(key));
    default:
      JSG_FAIL_REQUIRE(TypeError, "Unsupported key type");
  }
  KJ_UNREACHABLE;
}

ZeroOnFree::~ZeroOnFree() noexcept(false) {
  OPENSSL_cleanse(inner.begin(), inner.size());
}

void checkPbkdfLimits(jsg::Lock& js, size_t iterations) {
  auto& limits = Worker::Isolate::from(js).getLimitEnforcer();
  KJ_IF_SOME(max, limits.checkPbkdfIterations(js, iterations)) {
    JSG_FAIL_REQUIRE(DOMNotSupportedError,
        kj::str("Pbkdf2 failed: iteration counts above ", max, " are not supported (requested ",
            iterations, ")."));
  }
}

kj::Maybe<kj::Own<BIGNUM>> toBignum(kj::ArrayPtr<const kj::byte> data) {
  BIGNUM* result = BN_bin2bn(data.begin(), data.size(), nullptr);
  if (result == nullptr) return kj::none;
  return kj::Own<BIGNUM>(result, workerd::api::SslDisposer<BIGNUM, &BIGNUM_free>::INSTANCE);
}

BIGNUM* toBignumUnowned(kj::ArrayPtr<const kj::byte> data) {
  return BN_bin2bn(data.begin(), data.size(), nullptr);
}

kj::Maybe<kj::Array<kj::byte>> bignumToArray(const BIGNUM& n) {
  auto result = kj::heapArray<kj::byte>(BN_num_bytes(&n));
  if (BN_bn2bin(&n, result.begin()) != result.size()) return kj::none;
  return kj::mv(result);
}

kj::Maybe<kj::Array<kj::byte>> bignumToArrayPadded(const BIGNUM& n) {
  auto result = kj::heapArray<kj::byte>(BN_num_bytes(&n));
  if (BN_bn2binpad(&n, result.begin(), result.size()) != result.size()) return kj::none;
  return kj::mv(result);
}

kj::Maybe<kj::Array<kj::byte>> bignumToArrayPadded(const BIGNUM& n, size_t paddedLength) {
  auto result = kj::heapArray<kj::byte>(paddedLength);
  if (BN_bn2bin_padded(result.begin(), paddedLength, &n) == 0) {
    return kj::none;
  }
  return kj::mv(result);
}

kj::Maybe<jsg::BufferSource> bignumToArray(jsg::Lock& js, const BIGNUM& n) {
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, BN_num_bytes(&n));
  if (BN_bn2bin(&n, backing.asArrayPtr().begin()) != backing.size()) return kj::none;
  return jsg::BufferSource(js, kj::mv(backing));
}

kj::Maybe<jsg::BufferSource> bignumToArrayPadded(jsg::Lock& js, const BIGNUM& n) {
  auto result = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, BN_num_bytes(&n));
  if (BN_bn2binpad(&n, result.asArrayPtr().begin(), result.size()) != result.size()) {
    return kj::none;
  }
  return jsg::BufferSource(js, kj::mv(result));
}

kj::Maybe<jsg::BufferSource> bignumToArrayPadded(
    jsg::Lock& js, const BIGNUM& n, size_t paddedLength) {
  auto result = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, paddedLength);
  if (BN_bn2bin_padded(result.asArrayPtr().begin(), paddedLength, &n) == 0) {
    return kj::none;
  }
  return jsg::BufferSource(js, kj::mv(result));
}

kj::Own<BIGNUM> newBignum() {
  return kj::Own<BIGNUM>(BN_new(), workerd::api::SslDisposer<BIGNUM, &BIGNUM_free>::INSTANCE);
}

void CryptoKey::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

bool CSPRNG(kj::ArrayPtr<kj::byte> buffer) {
  do {
    if (1 == RAND_status())
      if (1 == RAND_bytes(buffer.begin(), buffer.size())) return true;
#if OPENSSL_VERSION_MAJOR >= 3
    const auto code = ERR_peek_last_error();
    // A misconfigured OpenSSL 3 installation may report 1 from RAND_poll()
    // and RAND_status() but fail in RAND_bytes() if it cannot look up
    // a matching algorithm for the CSPRNG.
    if (ERR_GET_LIB(code) == ERR_LIB_RAND) {
      const auto reason = ERR_GET_REASON(code);
      if (reason == RAND_R_ERROR_INSTANTIATING_DRBG || reason == RAND_R_UNABLE_TO_FETCH_DRBG ||
          reason == RAND_R_UNABLE_TO_CREATE_DRBG) {
        return false;
      }
    }
#endif
  } while (1 == RAND_poll());

  return false;
}

kj::Maybe<kj::ArrayPtr<const kj::byte>> tryGetAsn1Sequence(kj::ArrayPtr<const kj::byte> data) {
  if (data.size() < 2 || data[0] != 0x30) return kj::none;

  if (data[1] & 0x80) {
    // Long form.
    size_t n_bytes = data[1] & ~0x80;
    if (n_bytes + 2 > data.size() || n_bytes > sizeof(size_t)) return kj::none;
    size_t length = 0;
    for (size_t i = 0; i < n_bytes; i++) length = (length << 8) | data[i + 2];
    auto start = 2 + n_bytes;
    auto end = start + kj::min(data.size() - 2 - n_bytes, length);
    return data.slice(start, end);
  }

  // Short form.
  auto start = 2;
  auto end = start + kj::min(data.size() - 2, data[1]);
  return data.slice(start, end);
}

kj::Maybe<kj::Array<kj::byte>> simdutfBase64UrlDecode(kj::StringPtr input) {
  auto size = simdutf::maximal_binary_length_from_base64(input.begin(), input.size());
  auto buf = kj::heapArray<kj::byte>(size);
  auto result = simdutf::base64_to_binary(
      input.begin(), input.size(), buf.asChars().begin(), simdutf::base64_url);
  if (result.error != simdutf::SUCCESS) return kj::none;
  KJ_ASSERT(result.count <= size);
  return buf.slice(0, result.count).attach(kj::mv(buf));
}

kj::Maybe<jsg::BufferSource> simdutfBase64UrlDecode(jsg::Lock& js, kj::StringPtr input) {
  auto size = simdutf::maximal_binary_length_from_base64(input.begin(), input.size());
  KJ_STACK_ARRAY(kj::byte, buf, size, 1024, 4096);
  auto result = simdutf::base64_to_binary(
      input.begin(), input.size(), buf.asChars().begin(), simdutf::base64_url);
  if (result.error != simdutf::SUCCESS) return kj::none;
  KJ_ASSERT(result.count <= size);

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, result.count);
  backing.asArrayPtr().copyFrom(buf.first(result.count));
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource simdutfBase64UrlDecodeChecked(
    jsg::Lock& js, kj::StringPtr input, kj::StringPtr error) {
  return JSG_REQUIRE_NONNULL(simdutfBase64UrlDecode(js, input), Error, error);
}
}  // namespace workerd::api
