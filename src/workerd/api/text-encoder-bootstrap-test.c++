// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// When the per-isolate bootstrap loads the TypeScript TextEncoder (src/per_isolate/text-encoder.ts)
// under the TS_TEXT_ENCODER autogate: on first use of the global, and not before.
// encoding-ts-text-encoder-test covers what JavaScript sees of the class.

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {

WD_STRONG_BOOL(SpecCompliantPropertyAttributes);

namespace {

constexpr auto kSpecifier = "text-encoder"_kj;

void runWith(SpecCompliantPropertyAttributes spec, kj::Function<void(jsg::Lock& js)> body) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setSpecCompliantPropertyAttributes(spec == SpecCompliantPropertyAttributes::YES);
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .autogateKeys = kj::arr(util::AutogateKey::TS_TEXT_ENCODER),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) { body(env.js); });
}

jsg::JsValue eval(jsg::Lock& js, kj::StringPtr code) {
  return jsg::NonModuleScript::compile(js, code, "text-encoder-test.js"_kj).runAndReturn(js);
}

bool isLoaded(jsg::Lock& js) {
  return tryGetBootstrapExport(js, kSpecifier) != kj::none;
}

jsg::JsValue getExport(jsg::Lock& js) {
  auto exports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, kSpecifier));
  return KJ_ASSERT_NONNULL(exports.tryCast<jsg::JsObject>()).get(js, "TextEncoder"_kj);
}

// The global's place among the global scope prototype's keys, which the TypeScript class takes
// over from the C++ one.
constexpr auto kKeyIndex =
    "Reflect.ownKeys(Object.getPrototypeOf(globalThis)).indexOf('TextEncoder')"_kj;

KJ_TEST("reading the global loads TextEncoder, in the C++ class's place") {
  runWith(SpecCompliantPropertyAttributes::NO, [](jsg::Lock& js) {
    KJ_EXPECT(!isLoaded(js));
    // Neither looking for the global nor listing the global scope's names reads it.
    auto index = eval(js, kKeyIndex);
    KJ_EXPECT(eval(js, "'TextEncoder' in globalThis"_kj).isTrue());
    KJ_EXPECT(!isLoaded(js));

    KJ_EXPECT(eval(js, "new TextEncoder().encode('hi').length"_kj).strictEquals(js.num(2)));
    KJ_EXPECT(isLoaded(js));
    KJ_EXPECT(eval(js, "TextEncoder"_kj).strictEquals(getExport(js)));
    KJ_EXPECT(eval(js, kKeyIndex).strictEquals(index));
  });
}

KJ_TEST("under spec_compliant_property_attributes, both globals hold the one class") {
  runWith(SpecCompliantPropertyAttributes::YES, [](jsg::Lock& js) {
    KJ_EXPECT(!isLoaded(js));
    KJ_EXPECT(eval(js, R"js(
      const own = Object.getOwnPropertyDescriptor(globalThis, 'TextEncoder');
      const inherited =
          Object.getOwnPropertyDescriptor(Object.getPrototypeOf(globalThis), 'TextEncoder');
      [own.value === inherited.value, own.enumerable, inherited.enumerable].join()
    )js"_kj)
                  .strictEquals(js.str("true,false,true"_kj)));
    KJ_EXPECT(isLoaded(js));
    KJ_EXPECT(eval(js, "TextEncoder"_kj).strictEquals(getExport(js)));
  });
}

KJ_TEST("assigning the global before reading it loads nothing") {
  runWith(SpecCompliantPropertyAttributes::NO, [](jsg::Lock& js) {
    // As for any writable data property of the global scope's prototype, the assignment
    // defines an own property of the global, which shadows it.
    KJ_EXPECT(eval(js, R"js(
      globalThis.TextEncoder = 1;
      const own = Object.getOwnPropertyDescriptor(globalThis, 'TextEncoder');
      [TextEncoder, own.writable, own.enumerable, own.configurable].join()
    )js"_kj)
                  .strictEquals(js.str("1,true,true,true"_kj)));
    KJ_EXPECT(!isLoaded(js));
  });
}

KJ_TEST("TextEncoder loads after Object.prototype gains the fields of a property descriptor") {
  runWith(SpecCompliantPropertyAttributes::NO, [](jsg::Lock& js) {
    // The module's top level runs here, after user code: a descriptor that inherits from
    // Object.prototype would be invalid, and it must not call the global
    // Object.defineProperty().
    KJ_EXPECT(eval(js, R"js(
      const { defineProperty } = Object;
      const fields = ['get', 'set', 'value', 'writable', 'enumerable', 'configurable'];
      for (const field of fields) {
        defineProperty(Object.prototype, field, {
          __proto__: null,
          value: undefined,
          writable: true,
          configurable: true,
        });
      }
      Object.defineProperty = () => {
        throw new Error('the global Object.defineProperty() was called');
      };
      try {
        new TextEncoder().encode('hi').length
      } finally {
        Object.defineProperty = defineProperty;
        for (const field of fields) delete Object.prototype[field];
      }
    )js"_kj)
                  .strictEquals(js.num(2)));
    KJ_EXPECT(isLoaded(js));
  });
}

}  // namespace
}  // namespace workerd::api
