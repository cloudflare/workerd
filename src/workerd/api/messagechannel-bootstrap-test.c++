// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// When the per-isolate bootstrap loads MessageChannel and MessagePort
// (src/per_isolate/messagechannel/): on first use, through the globals or
// cloudflare-internal:messagechannel, and not before. src/tests/events/messagechannel/ covers what
// JavaScript sees of the classes.

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {

WD_STRONG_BOOL(ExposeGlobals);

namespace {

constexpr auto kSpecifier = "messagechannel/messagechannel"_kj;

void runWith(ExposeGlobals exposeGlobals, kj::Function<void(jsg::Lock& js)> body) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setExposeGlobalMessageChannel(exposeGlobals == ExposeGlobals::YES);
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) { body(env.js); });
}

jsg::JsValue eval(jsg::Lock& js, kj::StringPtr code) {
  return jsg::NonModuleScript::compile(js, code, "messagechannel-test.js"_kj).runAndReturn(js);
}

bool isLoaded(jsg::Lock& js) {
  return tryGetBootstrapExport(js, kSpecifier) != kj::none;
}

jsg::JsValue getExport(jsg::Lock& js, kj::StringPtr name) {
  auto exports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, kSpecifier));
  return KJ_ASSERT_NONNULL(exports.tryCast<jsg::JsObject>()).get(js, name);
}

KJ_TEST("reading a global loads MessageChannel and MessagePort") {
  runWith(ExposeGlobals::YES, [](jsg::Lock& js) {
    KJ_EXPECT(!isLoaded(js));
    // Neither looking for the globals nor listing the global scope's names reads them.
    KJ_EXPECT(eval(js, R"js(
      'MessageChannel' in globalThis &&
          Object.getOwnPropertyNames(Object.getPrototypeOf(globalThis)).includes('MessagePort')
    )js"_kj)
                  .isTrue());
    KJ_EXPECT(!isLoaded(js));

    KJ_EXPECT(eval(js, "typeof MessagePort"_kj).strictEquals(js.str("function"_kj)));
    KJ_EXPECT(isLoaded(js));
    KJ_EXPECT(eval(js, "MessagePort"_kj).strictEquals(getExport(js, "MessagePort"_kj)));
    KJ_EXPECT(eval(js, "MessageChannel"_kj).strictEquals(getExport(js, "MessageChannel"_kj)));
    // cloudflare-internal:messagechannel hands out the same classes.
    KJ_EXPECT(requireBootstrapExport(js, kSpecifier)
                  .strictEquals(KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, kSpecifier))));
  });
}

KJ_TEST("assigning a global before reading it loads nothing") {
  runWith(ExposeGlobals::YES, [](jsg::Lock& js) {
    // As for any writable data property of the global scope's prototype, the assignment
    // defines an own property of the global, which shadows it.
    KJ_EXPECT(eval(js, R"js(
      globalThis.MessageChannel = 1;
      const own = Object.getOwnPropertyDescriptor(globalThis, 'MessageChannel');
      [MessageChannel, own.writable, own.enumerable, own.configurable].join()
    )js"_kj)
                  .strictEquals(js.str("1,true,true,true"_kj)));
    KJ_EXPECT(!isLoaded(js));
  });
}

KJ_TEST("cloudflare-internal:messagechannel loads the classes without the globals") {
  runWith(ExposeGlobals::NO, [](jsg::Lock& js) {
    KJ_EXPECT(
        eval(js, "'MessageChannel' in globalThis || 'MessagePort' in globalThis"_kj).isFalse());
    KJ_EXPECT(!isLoaded(js));

    auto exports =
        KJ_ASSERT_NONNULL(requireBootstrapExport(js, kSpecifier).tryCast<jsg::JsObject>());
    KJ_EXPECT(isLoaded(js));
    KJ_EXPECT(exports.get(js, "MessageChannel"_kj).tryCast<jsg::JsFunction>() != kj::none);
    KJ_EXPECT(exports.get(js, "MessagePort"_kj).tryCast<jsg::JsFunction>() != kj::none);
  });
}

KJ_TEST("MessagePort loaded after EventTarget.prototype is replaced still extends EventTarget") {
  runWith(ExposeGlobals::YES, [](jsg::Lock& js) {
    // `extends` would read the replaced prototype, and a primitive would make it throw.
    KJ_EXPECT(eval(js, R"js(
      const saved = EventTarget.prototype;
      EventTarget.prototype = 5;
      try {
        const replaced = EventTarget.prototype === 5;
        const { port1 } = new MessageChannel();
        [
          replaced,
          Object.getPrototypeOf(MessagePort.prototype) === saved,
          Object.getPrototypeOf(MessagePort) === EventTarget,
          typeof port1.addEventListener,
        ].join()
      } finally {
        EventTarget.prototype = saved;
      }
    )js"_kj)
                  .strictEquals(js.str("true,true,true,function"_kj)));
  });
}

}  // namespace
}  // namespace workerd::api
