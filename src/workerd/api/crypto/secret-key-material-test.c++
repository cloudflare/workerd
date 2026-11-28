// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"
#include "impl.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct CryptoContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(CryptoContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CryptoIsolate, CryptoContext);

// Imports `raw` as a non-extractable key with `importFunc` and returns its secret key material.
kj::Array<kj::byte> secretOf(jsg::Lock& js,
    CryptoKey::Impl::ImportFunc importFunc,
    kj::StringPtr name,
    kj::ArrayPtr<const kj::byte> raw,
    kj::ArrayPtr<const kj::String> usages) {
  auto data = jsg::JsBufferSource(jsg::JsUint8Array::create(js, raw));
  auto impl = importFunc(js, name, "raw", data.addRef(js),
      SubtleCrypto::ImportKeyAlgorithm{.name = kj::str(name)}, false, usages);
  auto key = js.alloc<CryptoKey>(kj::mv(impl));
  KJ_EXPECT(!key->getExtractable());
  return kj::heapArray(KJ_ASSERT_NONNULL(key->getSecretKeyMaterial()));
}

KJ_TEST("getSecretKeyMaterial() returns a non-extractable secret key's bytes") {
  jsg::test::Evaluator<CryptoContext, CryptoIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](CryptoIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<CryptoContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      auto raw = kj::heapArray<kj::byte>(32);
      for (auto i: kj::indices(raw)) raw[i] = i;

      auto hkdfUsages = kj::arr(kj::str("deriveBits"));
      KJ_EXPECT(secretOf(js, CryptoKey::Impl::importHkdf, "HKDF", raw, hkdfUsages) == raw);

      auto aesUsages = kj::arr(kj::str("encrypt"));
      KJ_EXPECT(secretOf(js, CryptoKey::Impl::importAes, "AES-GCM", raw, aesUsages) == raw);
    });
  });
}

KJ_TEST("getSecretKeyMaterial() returns none for a key without raw secret material") {
  class OpaqueKey final: public CryptoKey::Impl {
   public:
    OpaqueKey(): Impl(false, CryptoKeyUsageSet::sign()) {}

    kj::StringPtr getAlgorithmName() const override {
      return "test-opaque";
    }
    CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock&) const override {
      return CryptoKey::KeyAlgorithm{"test-opaque"};
    }
    bool equals(const Impl& other) const override {
      return this == &other;
    }
  };

  OpaqueKey key;
  KJ_EXPECT(key.getSecretKeyMaterial() == kj::none);
}

}  // namespace
}  // namespace workerd::api
