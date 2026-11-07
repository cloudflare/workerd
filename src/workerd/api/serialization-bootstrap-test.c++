// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The per-isolate bootstrap's serialization helpers (src/per_isolate/serialization.ts), run
// against jsg::Serializer and jsg::Deserializer through structuredClone().

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/script.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/strong-bool.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {

WD_STRONG_BOOL(EnhancedErrorSerialization);

namespace {

using Callback = jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>&)>;

// The bootstrap's modules, plus a module "test" whose exports the script sets, so that the classes
// the script defines can be found by their deserializeInfo.
struct TestModules final: public jsg::BootstrapModules {
  explicit TestModules(jsg::BootstrapModules& bootstrap): bootstrap(bootstrap) {}

  kj::Maybe<jsg::JsValue> tryGetLoadedExports(jsg::Lock& js, kj::StringPtr specifier) override {
    if (specifier == "test"_kj) {
      KJ_IF_SOME(e, exports) {
        return e.getHandle(js);
      }
      return kj::none;
    }
    return bootstrap.tryGetLoadedExports(js, specifier);
  }

  jsg::BootstrapModules& bootstrap;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> exports;
};

// Runs `body` as the body of a function of `helpers` (the exports of the bootstrap's serialization
// module), `setTestExports` and `roundTrip(value, preserveStackInErrors)`, returning its result as
// a string.
kj::String runScript(kj::StringPtr body, EnhancedErrorSerialization enhancedErrorSerialization) {
  // The bootstrap loads the serialization module whatever the compatibility flags are.
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  if (enhancedErrorSerialization) {
    flags.setEnhancedErrorSerialization(true);
    flags.setWorkerdExperimental(true);
  }
  TestFixture fixture({.featureFlags = flags.asReader()});

  kj::String result;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto context = js.v8Context();
    auto helpers = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "serialization"_kj),
        "the bootstrap did not load the serialization module");

    auto& bootstrap =
        KJ_ASSERT_NONNULL(jsg::getAlignedPointerFromEmbedderData<jsg::BootstrapModules>(
            context, jsg::ContextPointerSlot::BOOTSTRAP_STATE));
    TestModules modules(bootstrap);
    jsg::setAlignedPointerInEmbedderData(context, jsg::ContextPointerSlot::BOOTSTRAP_STATE,
        static_cast<jsg::BootstrapModules*>(&modules));
    KJ_DEFER(jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::BOOTSTRAP_STATE, &bootstrap));

    auto setTestExports = js.wrapReturningFunction(context,
        Callback([&modules](jsg::Lock& js,
                     const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
      modules.exports = jsg::JsRef<jsg::JsValue>(js, jsg::JsValue(info[0]));
      return js.undefined();
    }));

    auto roundTrip = js.wrapReturningFunction(context,
        Callback([](jsg::Lock& js,
                     const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
      auto content = ({
        jsg::Serializer ser(js);
        ser.write(js, jsg::JsValue(info[0]));
        ser.release();
      });
      jsg::Deserializer deser(js, content.data, kj::none, kj::none,
          jsg::Deserializer::Options{.preserveStackInErrors = info[1]->IsTrue()});
      return deser.readValue(js);
    }));

    auto code = kj::str("((helpers, setTestExports, roundTrip) => {", body, "})");
    auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "serialization-test.js"_kj)
                                    .runAndReturn(js)
                                    .tryCast<jsg::JsFunction>());
    result =
        fn.call(js, js.undefined(), helpers, jsg::JsValue(setTestExports), jsg::JsValue(roundTrip))
            .toString(js);
  });
  return result;
}

// Classes set up with the helpers, as the bootstrap's own are.
constexpr kj::StringPtr CLASSES = R"js(
  const { markUncloneable, markCloneable, makeCloneable } = helpers;
  let constructed = 0;

  class Point {
    constructor(x) { constructed++; markCloneable(this, pointCloneable); this.x = x; }
  }
  const pointCloneable =
      makeCloneable(Point, 'test:Point', (p) => p.x, (p, data) => { p.x = data + 1; });

  class TestError extends Error {
    constructor(message) { super(message); markCloneable(this, testErrorCloneable); }
  }
  const testErrorCloneable = makeCloneable(TestError, 'test:TestError', (e) => e.message,
      (e, data) => {
    Object.defineProperty(e, 'message', { value: data, writable: true, configurable: true });
  });

  class Opaque {
    constructor() { markUncloneable(this); }
  }

  setTestExports({ Point, TestError });
)js"_kj;

void expectScript(kj::StringPtr body,
    kj::StringPtr expected,
    EnhancedErrorSerialization enhancedErrorSerialization = EnhancedErrorSerialization::NO) {
  auto result = runScript(kj::str(CLASSES, body), enhancedErrorSerialization);
  KJ_EXPECT(result == expected, result, expected);
}

KJ_TEST("An instance of a cloneable class round-trips without its constructor") {
  expectScript(R"js(
    const p = structuredClone(new Point(1));
    return [p instanceof Point, Object.getPrototypeOf(p) === Point.prototype, p.x, constructed,
        structuredClone(p).x].join();
  )js"_kj,
      "true,true,2,1,3"_kj);
}

KJ_TEST("The class that marked an instance decides how it is serialized") {
  expectScript(R"js(
    class Other {}
    const p = new Point(1);
    Object.setPrototypeOf(p, Other.prototype);
    const copy = structuredClone(p);
    return [Object.getPrototypeOf(copy) === Point.prototype, copy.x].join();
  )js"_kj,
      "true,2"_kj);
}

KJ_TEST("An instance of a cloneable class inheriting from Error is a native error") {
  expectScript(R"js(
    const e = structuredClone(new TestError('boom'));
    return [e instanceof TestError, Object.prototype.toString.call(e), e.message].join();
  )js"_kj,
      "true,[object Error],boom"_kj);
}

// Whether an instance of TestError, and a native error, keep the writer's stack when read with
// preserveStackInErrors, then without, then through structuredClone().
constexpr kj::StringPtr KEEPS_STACKS = R"js(
  const keepsStack = (error, read) => {
    error.stack = 'sender stack';
    return read(error).stack === 'sender stack';
  };
  const reads = [(e) => roundTrip(e, true), (e) => roundTrip(e, false), structuredClone];
  return reads.map((read) =>
      `${keepsStack(new TestError('boom'), read)} ${keepsStack(new Error('boom'), read)}`).join();
)js"_kj;

KJ_TEST("An instance of a cloneable class inheriting from Error keeps a native error's stack") {
  // As a native error does, it loses the writer's stack only without preserveStackInErrors.
  expectScript(KEEPS_STACKS, "true true,false false,true true"_kj, EnhancedErrorSerialization::YES);
  // Without enhanced error serialization, native errors keep their stacks whatever the options.
  expectScript(KEEPS_STACKS, "true true,true true,true true"_kj);
}

KJ_TEST("getOwnDataProperty() reads neither the prototype chain nor an accessor") {
  expectScript(R"js(
    const { getOwnDataProperty } = helpers;
    let getterCalls = 0;
    const getter = { get() { getterCalls++; return 'getter'; }, configurable: true };
    Object.defineProperty(Object.prototype, 'b', getter);
    Object.defineProperty(Object.prototype, 'value', getter);
    try {
      // Data that lacks `b`, as an untrusted peer may send, read in deserialize().
      class Pair {
        constructor(a) { markCloneable(this, pairCloneable); this.fields = [a]; }
      }
      const pairCloneable = makeCloneable(Pair, 'test:Pair', (p) => ({ a: p.fields[0] }),
          (p, data) => {
        p.fields = [getOwnDataProperty(data, 'a'), getOwnDataProperty(data, 'b')];
      });
      setTestExports({ Pair });
      const copy = structuredClone(new Pair(1));
      const accessor = getOwnDataProperty({ get a() { getterCalls++; return 1; } }, 'a');
      return [...copy.fields, accessor, getterCalls].join();
    } finally {
      delete Object.prototype.b;
      delete Object.prototype.value;
    }
  )js"_kj,
      "1,,,0"_kj);
}

KJ_TEST("An uncloneable instance throws DataCloneError") {
  expectScript(R"js(
    try {
      structuredClone(new Opaque());
      return 'cloned';
    } catch (e) {
      return e.name + ': ' + e.message;
    }
  )js"_kj,
      "DataCloneError: Could not serialize object of type \"Opaque\". This type does not support "
      "serialization."_kj);
}

}  // namespace
}  // namespace workerd::api
