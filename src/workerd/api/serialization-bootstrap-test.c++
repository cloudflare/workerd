// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The per-isolate bootstrap's serialization helpers (src/per_isolate/serialization.ts), run
// against jsg::Serializer and jsg::Deserializer through structuredClone().

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

using Callback = jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>&)>;

// Runs `body` as the body of a function of `helpers` (the exports of the bootstrap's serialization
// module) and `setTestExports`, returning its result as a string. setTestExports() makes its
// argument the exports of a bootstrap module "test", so that the deserializer finds the classes
// the script defines by their deserializeInfo.
kj::String runScript(kj::StringPtr body) {
  // The bootstrap loads the serialization module whatever the compatibility flags are.
  TestFixture fixture;

  kj::String result;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto context = js.v8Context();
    auto helpers = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "serialization"_kj),
        "the bootstrap did not load the serialization module");

    auto setTestExports = js.wrapReturningFunction(context,
        Callback([](jsg::Lock& js,
                     const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
      setBootstrapExportForTest(js, "test"_kj, jsg::JsValue(info[0]));
      return js.undefined();
    }));

    auto code = kj::str("((helpers, setTestExports) => {", body, "})");
    auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "serialization-test.js"_kj)
                                    .runAndReturn(js)
                                    .tryCast<jsg::JsFunction>());
    result = fn.call(js, js.undefined(), helpers, jsg::JsValue(setTestExports)).toString(js);
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

  class Opaque {
    constructor() { markUncloneable(this); }
  }

  setTestExports({ Point });
)js"_kj;

void expectScript(kj::StringPtr body, kj::StringPtr expected) {
  auto result = runScript(kj::str(CLASSES, body));
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

KJ_TEST("A class inheriting from a built-in that V8 serializes itself cannot be cloneable") {
  // V8 serializes the instances itself, whatever their marks: an uncloneable Map is cloned as a
  // Map, so makeCloneable() rejects such a class rather than have it cloned as the built-in.
  expectScript(R"js(
    class UncloneableMap extends Map {
      constructor() { super([[1, 2]]); markUncloneable(this); }
    }
    const copy = structuredClone(new UncloneableMap());
    const results = [`${Object.getPrototypeOf(copy) === Map.prototype} ${copy.get(1)}`];
    for (const base of [Array, ArrayBuffer, SharedArrayBuffer, Uint8Array, Float64Array, DataView,
        Date, Map, Set, RegExp, Boolean, Number, String]) {
      class Derived extends base {}
      class Further extends Derived {}
      try {
        makeCloneable(Further, 'test:Further', () => 0, () => {});
        results.push(`${base.name} accepted`);
      } catch (e) {
        if (!(e instanceof TypeError)) throw e;
      }
    }
    return results.join();
  )js"_kj,
      "true 2"_kj);
}

KJ_TEST("A class inheriting from Error cannot be cloneable") {
  // The deserializer makes an ordinary object, never an error.
  expectScript(R"js(
    class DerivedError extends TypeError {}
    try {
      makeCloneable(DerivedError, 'test:DerivedError', () => 0, () => {});
      return 'accepted';
    } catch (e) {
      return `${e.name}: ${e.message}`;
    }
  )js"_kj,
      "TypeError: test:DerivedError inherits from Error, so it cannot be cloneable."_kj);
}

}  // namespace
}  // namespace workerd::api
