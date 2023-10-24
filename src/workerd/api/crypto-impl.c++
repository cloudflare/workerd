// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto-impl.h"
#include "util.h"
#include <algorithm>
#include <map>
#include <openssl/err.h>
#include <openssl/ec.h>
#include <openssl/rsa.h>
#include <openssl/crypto.h>

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

  return kj::str(": ", kj::strArray(KJ_MAP(e, accumulatedErrors) {
    KJ_SWITCH_ONEOF(accumulatedErrors[0]) {
      KJ_CASE_ONEOF(e, kj::StringPtr) {
        return e;
      }
      KJ_CASE_ONEOF(e, OpensslUntranslatedError) {
        return kj::StringPtr(e.reasonName);
      }
    }
    KJ_UNREACHABLE;
  }, " "), ".");
}
}

SslArrayDisposer SslArrayDisposer::INSTANCE;

void SslArrayDisposer::disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                  size_t capacity, void (*destroyElement)(void*)) const {
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
  switch(ERR_GET_LIB(ERR_peek_last_error())) {
    // The error code defines overlap between the different boringssl libraries (for example, we
    // have EC_R_INVALID_ENCODING == RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY), so we must check the
    // library code.
    case ERR_LIB_EC:
      switch (ERR_GET_REASON(ERR_peek_last_error())) {
#define MAP_ERROR(CODE, TEXT) \
        case CODE: \
          ERR_clear_error(); \
          kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, file, line, \
              kj::str(JSG_EXCEPTION(DOMOperationError) ": ", TEXT)));

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
    char message[1024];
    ERR_error_string_n(error, message, sizeof(message));
    lines.add(kj::heapString(message));
  }
  kj::throwFatalException(kj::Exception(kj::Exception::Type::FAILED, file, line, kj::str(
      "OpenSSL call failed: ", code, "; ",
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

      return OpensslUntranslatedError {
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
      [](const auto& error) {
        return error.template is<OpensslUntranslatedError>();
      });

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

ZeroOnFree::~ZeroOnFree() noexcept(false) {
  OPENSSL_cleanse(inner.begin(), inner.size());
}

}  // namespace workerd::api
