// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"

#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/rsa.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("Crypto error conversion") {
  ClearErrorOnReturn clearErrorOnReturn;

  // Intentionally provide an error type not handled in throwOpensslError()
  // (RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY) that overlaps with an EC error that we do handle
  // (EC_R_INVALID_ENCODING). This test will fail if we do not check the library code of the error.
  // This test needs to be updated (e.g. with a different error code) if
  // RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY is added to the error types provided to users in
  // throwOpensslError().

  OPENSSL_PUT_ERROR(RSA, RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY);
  // Throw an exception based on BoringSSL error queue, expecting to get an internal error instead
  // of a DOMException
  KJ_EXPECT_THROW_MESSAGE("OpenSSL call failed", OSSLCALL(0));

  // EC_R_INVALID_ENCODING is one of the errors converted to user errors, test that it is converted
  // to a DOMException.
  OPENSSL_PUT_ERROR(EC, EC_R_INVALID_ENCODING);
  KJ_EXPECT_THROW_MESSAGE("jsg.DOMException(OperationError): Invalid point encoding.", OSSLCALL(0));
}

KJ_TEST("RSA_R_KEY_SIZE_TOO_SMALL is a user-facing error") {
  ClearErrorOnReturn clearErrorOnReturn;

  // RSA_R_KEY_SIZE_TOO_SMALL should be converted to a DOMException(OperationError) rather than
  // an internal error (which would generate Sentry noise).
  OPENSSL_PUT_ERROR(RSA, RSA_R_KEY_SIZE_TOO_SMALL);
  KJ_EXPECT_THROW_MESSAGE(
      "jsg.DOMException(OperationError): RSA key size is too small.", OSSLCALL(0));
}

KJ_TEST("RSA_R_INTERNAL_ERROR is a user-facing error") {
  ClearErrorOnReturn clearErrorOnReturn;

  // RSA_R_INTERNAL_ERROR should be converted to a DOMException(OperationError) rather than
  // an internal error. This error occurs during RSA signing when the private key computation
  // or post-sign verification fails (e.g. due to corrupted key material).
  OPENSSL_PUT_ERROR(RSA, RSA_R_INTERNAL_ERROR);
  KJ_EXPECT_THROW_MESSAGE("jsg.DOMException(OperationError): RSA operation failed.", OSSLCALL(0));
}

}  // namespace
}  // namespace workerd::api
