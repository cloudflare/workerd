// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>

#include "crypto-impl.h"
#include "crypto.h"
#include <workerd/api/util.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/jsg-test.h>
#include <array>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct CryptoContext : public jsg::Object {
  JSG_RESOURCE_TYPE(CryptoContext) {
  }
};
JSG_DECLARE_ISOLATE_TYPE(CryptoIsolate, CryptoContext);

KJ_TEST("RSASSA-PKCS1-v1_5 generateKey infinite loop") {
  // Basic regression test for a case where generateKey for RSA-OAEP hangs in an infinite loop.
  SubtleCrypto::GenerateKeyAlgorithm algorithm;
  algorithm.name = kj::str("RSASSA-PKCS1-v1_5");
  algorithm.hash = kj::str("SHA-256");
  algorithm.modulusLength = 1024;
  algorithm.publicExponent = kj::arrOf<kj::byte>(1);
  KJ_EXPECT_THROW_MESSAGE("expected *v == 3 || *v == 65537",
      CryptoKey::Impl::generateRsa("RSASSA-PKCS1-v1_5", kj::mv(algorithm), false,
      kj::arr(kj::str("sign"), kj::str("verify"))));
}

KJ_TEST("RSA-PSS generateKey infinite loop") {
  // Basic regression test for a case where generateKey for RSA-OAEP hangs in an infinite loop.
  SubtleCrypto::GenerateKeyAlgorithm algorithm;
  algorithm.name = kj::str("RSA-PSS");
  algorithm.hash = kj::str("SHA-256");
  algorithm.modulusLength = 1024;
  algorithm.publicExponent = kj::arrOf<kj::byte>(1);
  KJ_EXPECT_THROW_MESSAGE("expected *v == 3 || *v == 65537",
      CryptoKey::Impl::generateRsa("RSA-PSS", kj::mv(algorithm), false,
      kj::arr(kj::str("sign"), kj::str("verify"))));
}

KJ_TEST("EDDSA ED25519 generateKey") {
  jsg::test::Evaluator<CryptoContext, CryptoIsolate> e(v8System);
  CryptoIsolate &cryptoIsolate = e.getIsolate();
  jsg::V8StackScope stackScope;
  CryptoIsolate::Lock isolateLock(cryptoIsolate, stackScope);
  auto isolate = isolateLock.v8Isolate;
  v8::HandleScope handleScope(isolate);
  auto context = isolateLock.newContext<CryptoContext>().getHandle(isolate);
  v8::Context::Scope contextScope(context);

  SubtleCrypto::GenerateKeyAlgorithm algorithm;
  algorithm.name = kj::str("NODE-ED25519");
  algorithm.namedCurve = kj::str("NODE-ED25519");
  CryptoKey::Impl::generateEddsa("NODE-ED25519", kj::mv(algorithm), false,
                                 kj::arr(kj::str("sign"), kj::str("verify")));
}
}  // namespace
}  // namespace workerd::api
