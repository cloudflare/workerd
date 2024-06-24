// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>

#include "impl.h"

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>

namespace workerd::api {
namespace {

KJ_TEST("Crypto error conversion") {
  ERR_clear_error();
  // Intentionally provide an error type not handled in throwOpensslError()
  // (RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY) that overlaps with an EC error that we do handle
  // (EC_R_INVALID_ENCODING). This test will fail if we do not check the library code of the error.
  // This test needs to be updated (e.g. with a different error code) if
  // RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY is added to the error types provided to users in
  // throwOpensslError().

  OPENSSL_PUT_ERROR(RSA, RSA_R_CANNOT_RECOVER_MULTI_PRIME_KEY);
  // Throw an exception based on boringssl error queue, expecting to get an internal error instead
  // of a DOMException
  KJ_EXPECT_THROW_MESSAGE("OpenSSL call failed", OSSLCALL(0));

  // EC_R_INVALID_ENCODING is one of the errors converted to user errors, test that it is converted
  // to a DOMException.
  OPENSSL_PUT_ERROR(EC, EC_R_INVALID_ENCODING);
  KJ_EXPECT_THROW_MESSAGE("jsg.DOMException(OperationError): Invalid point encoding.", OSSLCALL(0));
}

}  // namespace
}  // namespace workerd::api
