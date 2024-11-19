// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"
#include "impl.h"

#include <workerd/api/util.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

#include <kj/test.h>

#include <array>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct CryptoContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(CryptoContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CryptoIsolate, CryptoContext);

KJ_TEST("AES-KW key wrap") {
  // Basic test that I wrote when I was seeing heap corruption. Found it easier to iterate on with
  // ASAN/valgrind than using our conformance tests with test-runner.
  jsg::test::Evaluator<CryptoContext, CryptoIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](CryptoIsolate::Lock& isolateLock) {
    auto rawWrappingKeys = std::array<kj::Array<kj::byte>, 3>({
      kj::heapArray<kj::byte>({0xe6, 0x95, 0xea, 0xe3, 0xa8, 0xc0, 0x30, 0xf1, 0x76, 0xe3, 0x0e,
        0x8e, 0x36, 0xf8, 0xf4, 0x31}),
      // AES-KW 128
      kj::heapArray<kj::byte>({0x20, 0xa7, 0x98, 0xd1, 0x82, 0x8c, 0x18, 0x67, 0xfd, 0xda, 0x16,
        0x03, 0x57, 0xc6, 0x32, 0x4f, 0xcc, 0xe8, 0x08, 0x6d, 0x21, 0xe9, 0x3c, 0x60}),
      // AES-KW 192
      kj::heapArray<kj::byte>({0x52, 0x4b, 0x67, 0x25, 0xe3, 0x56, 0xaa, 0xce, 0x7e, 0x76, 0x9b,
        0x48, 0x92, 0x55, 0x49, 0x06, 0x12, 0x5e, 0xf5, 0xae, 0xce, 0x39, 0xde, 0xc2, 0x5b, 0x27,
        0x33, 0x4e, 0x6e, 0x52, 0x32, 0x4e}),
      // AES-KW 256
    });

    auto aesKeys = KJ_MAP(rawKey, kj::mv(rawWrappingKeys)) {
      SubtleCrypto::ImportKeyAlgorithm algorithm = {
        .name = kj::str("AES-KW"),
      };
      bool extractable = false;

      return CryptoKey::Impl::importAes(isolateLock, "AES-KW", "raw", kj::mv(rawKey),
          kj::mv(algorithm), extractable, {kj::str("wrapKey"), kj::str("unwrapKey")});
    };

    auto keyMaterial = kj::heapArray<const kj::byte>(
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24});

    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<CryptoContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      for (const auto& aesKey: aesKeys) {
        SubtleCrypto::EncryptAlgorithm params;
        params.name = kj::str("AES-KW");

        auto wrapped = aesKey->wrapKey(js, kj::mv(params), keyMaterial.asPtr());

        params = {};
        params.name = kj::str("AES-KW");

        auto unwrapped = aesKey->unwrapKey(js, kj::mv(params), wrapped);

        KJ_EXPECT(unwrapped.asArrayPtr() == keyMaterial);

        // Corruption of wrapped key material should throw.
        params = {};
        params.name = kj::str("AES-KW");
        wrapped.asArrayPtr()[5] += 1;
        KJ_EXPECT_THROW_MESSAGE("[24 == -1]", aesKey->unwrapKey(js, kj::mv(params), wrapped));
      }
    });
  });
}

// Disable null pointer checks (a subset of UBSan) here due to the null reference being passed for
// jwkHandler. Using attribute push as annotating just the test itself didn't seem to work.
#if __clang__ && __has_feature(undefined_behavior_sanitizer)
#pragma clang attribute push(__attribute__((no_sanitize("null"))), apply_to = function)
#endif
KJ_TEST("AES-CTR key wrap") {
  // Basic test that let me repro an issue where using an AES key that's not AES-KW would fail to
  // wrap if it didn't have "encrypt" in its usages when created.

  const jsg::TypeHandler<SubtleCrypto::JsonWebKey>* jwkHandler = nullptr;
  // Not testing JWK here, so valid value isn't needed.

  static constexpr kj::byte RAW_KEY_DATA[] = {0x52, 0x4b, 0x67, 0x25, 0xe3, 0x56, 0xaa, 0xce, 0x7e,
    0x76, 0x9b, 0x48, 0x92, 0x55, 0x49, 0x06, 0x12, 0x5e, 0xf5, 0xae, 0xce, 0x39, 0xde, 0xc2, 0x5b,
    0x27, 0x33, 0x4e, 0x6e, 0x52, 0x32, 0x4e};

  static constexpr kj::ArrayPtr<const kj::byte> KEY_DATA(RAW_KEY_DATA, 32);

  SubtleCrypto subtle;

  static constexpr auto getWrappingKey = [](jsg::Lock& js, SubtleCrypto& subtle) {
    return subtle.importKeySync(js, "raw", kj::heapArray<kj::byte>(KEY_DATA),
        SubtleCrypto::ImportKeyAlgorithm{.name = kj::str("AES-CTR")}, false /* extractable */,
        {kj::str("wrapKey"), kj::str("unwrapKey")});
  };

  static constexpr auto getEnc = [] {
    return SubtleCrypto::EncryptAlgorithm{
      .name = kj::str("AES-CTR"),
      .counter = kj::arr<uint8_t>(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
      .length = 5,
    };
  };

  static constexpr auto getImportKeyAlg = [] {
    return SubtleCrypto::ImportKeyAlgorithm{
      .name = kj::str("AES-CBC"),
      .length = 256,
    };
  };

  jsg::test::Evaluator<CryptoContext, CryptoIsolate> e(v8System);
  bool completed = false;

  e.getIsolate().runInLockScope([&](CryptoIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<CryptoContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      auto wrappingKey = getWrappingKey(js, subtle);
      subtle
          .importKey(js, kj::str("raw"), kj::heapArray(KEY_DATA), getImportKeyAlg(), true,
              kj::arr(kj::str("decrypt")))
          .then(js,
              [&](jsg::Lock&, jsg::Ref<CryptoKey> toWrap) {
        return subtle.wrapKey(js, kj::str("raw"), *toWrap, *wrappingKey, getEnc(), *jwkHandler);
      })
          .then(js,
              [&](jsg::Lock&, jsg::BufferSource wrapped) {
        auto data = kj::heapArray(wrapped.asArrayPtr());
        return subtle.unwrapKey(js, kj::str("raw"), kj::mv(data), *wrappingKey, getEnc(),
            getImportKeyAlg(), true, kj::arr(kj::str("encrypt")), *jwkHandler);
      })
          .then(js, [&](jsg::Lock& js, jsg::Ref<CryptoKey> unwrapped) {
        return subtle.exportKey(js, kj::str("raw"), *unwrapped);
      }).then(js, [&](jsg::Lock&, api::SubtleCrypto::ExportKeyData roundTrippedKeyMaterial) {
        KJ_ASSERT(roundTrippedKeyMaterial.get<jsg::BufferSource>() == KEY_DATA);
        completed = true;
      });

      js.runMicrotasks();
    });
  });

  KJ_ASSERT(completed, "Microtasks did not run fully.");
}
#if __clang__ && __has_feature(undefined_behavior_sanitizer)
#pragma clang attribute pop  // __attribute__((no_sanitize("null"))
#endif

}  // namespace
}  // namespace workerd::api
