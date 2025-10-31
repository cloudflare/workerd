// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "jsvalue.h"
namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct JsValueContext: public ContextGlobalObject {
  JsRef<JsValue> persisted;
  JsValue takeJsValue(Lock& js, JsValue v) {
    KJ_ASSERT(!v.isTruthy(js));
    KJ_ASSERT(v.typeOf(js) == "boolean"_kj);
    KJ_ASSERT(v.isBoolean());
    KJ_ASSERT(!v.isObject());
    JsBoolean b = KJ_ASSERT_NONNULL(v.tryCast<JsBoolean>());
    KJ_ASSERT(!b.value(js));

    return b;
  }
  JsValue takeJsString(Lock& js, Optional<JsString> v) {
    return v.orDefault([&] { return js.str("bar"_kj); });
  }
  JsValue takeJsNumber(Lock& js, Optional<JsNumber> v) {
    return v.orDefault([&] { return js.num(42.0); });
  }
  JsBoolean takeJsBoolean(Lock& js, JsBoolean v, const TypeHandler<bool>& handler) {
    auto ref = v.addRef(js);

    // Because Js* types are trivially assignable to v8::Local<v8::Value>,
    // they work out of the box with the existing TypeHandler<T> model
    // and can be converted into more specific types easily.
    bool result = KJ_ASSERT_NONNULL(handler.tryUnwrap(js, v));
    KJ_ASSERT(result == v.value(js));

    return ref.getHandle(js);
  }
  JsObject takeJsObject(JsObject v) {
    return v;
  }
  JsArray takeJsArray(Lock& js, JsArray v) {
    KJ_ASSERT(v.size() == 3);
    JsValue val = v.get(js, 0);
    KJ_ASSERT(val.isNumber());
    KJ_ASSERT(kj::str(val) == "1");
    return v;
  }
  JsValue getString(Lock& js) {
    return js.str("foo"_kj);
  }
  JsValue getStringIntern(Lock& js) {
    return js.strIntern("foo");
  }
  JsMap getMap(Lock& js) {
    auto map = js.map();
    map.set(js, "foo", js.num(1));
    return map;
  }
  JsArray getArray(Lock& js) {
    return js.arr(js.undefined(), js.null(), js.num(1));
  }
  JsSet getSet(Lock& js) {
    return js.set(js.num(1), js.num(1), js.str("foo"_kj), js.str("foo"_kj));
  }
  void setRef(Lock& js, JsRef<JsString> value) {
    JsValue v = value.getHandle(js);
    persisted = v.addRef(js);
  }
  JsValue getRef(Lock& js) {
    return persisted.getHandle(js);
  }
  JsDate getDate(Lock& js) {
    return js.date(0);
  }

  JsValue callFunction(Lock& js, JsFunction fn) {
    return fn.callNoReceiver(js, js.num(1));
  }

  struct Foo: public Object {
    JSG_RESOURCE_TYPE(Foo) {}
  };

  JsValue checkProxyPrototype(Lock& js, JsValue value) {
    JSG_REQUIRE(value.isProxy(), TypeError, "not a proxy");
    auto obj = KJ_ASSERT_NONNULL(value.tryCast<JsObject>());
    return obj.getPrototype(js);
  }

  JSG_RESOURCE_TYPE(JsValueContext) {
    JSG_METHOD(takeJsValue);
    JSG_METHOD(takeJsString);
    JSG_METHOD(takeJsNumber);
    JSG_METHOD(takeJsBoolean);
    JSG_METHOD(takeJsObject);
    JSG_METHOD(takeJsArray);
    JSG_METHOD(getString);
    JSG_METHOD(getStringIntern);
    JSG_METHOD(getMap);
    JSG_METHOD(getArray);
    JSG_METHOD(getSet);
    JSG_METHOD(setRef);
    JSG_METHOD(getRef);
    JSG_METHOD(getDate);
    JSG_METHOD(checkProxyPrototype);
    JSG_METHOD(callFunction);
    JSG_NESTED_TYPE(Foo);
  }
};
JSG_DECLARE_ISOLATE_TYPE(JsValueIsolate, JsValueContext, JsValueContext::Foo);

KJ_TEST("simple") {
  Evaluator<JsValueContext, JsValueIsolate> e(v8System);
  e.expectEval("takeJsValue(false)", "boolean", "false");
  e.expectEval("takeJsString(123)", "string", "123");
  e.expectEval("takeJsString()", "string", "bar");
  e.expectEval("takeJsNumber(5)", "number", "5");
  e.expectEval("takeJsNumber()", "number", "42");
  // Empty string coerces to 0 value in JS. Ex: Number('') === 0
  e.expectEval("takeJsNumber('')", "number", "0");
  // NaN is still a JsNumber. To check "safety", call toSafeInteger() method.
  e.expectEval("takeJsNumber(NaN)", "number", "NaN");
  e.expectEval("Number({[Symbol.toPrimitive]() { return 1 }})", "number", "1");
  e.expectEval("takeJsBoolean(true)", "boolean", "true");
  e.expectEval("takeJsBoolean('hi')", "boolean", "true");
  e.expectEval("takeJsBoolean('')", "boolean", "false");
  e.expectEval("const o = {}; o === takeJsObject(o);", "boolean", "true");
  e.expectEval("const a = [1,2,3]; a[1] === takeJsArray(a)[1]", "boolean", "true");
  e.expectEval("getString()", "string", "foo");
  e.expectEval("getStringIntern()", "string", "foo");
  e.expectEval("const m = getMap(); m.get('foo')", "number", "1");
  e.expectEval("const s = getSet(); s.size === 2 && s.has(1) && s.has('foo') && !s.has('bar')",
      "boolean", "true");
  e.expectEval("const a = getArray(); a[2];", "number", "1");
  e.expectEval("setRef('foo'); getRef('foo')", "string", "foo");
  e.expectEval("takeJsObject(undefined)", "throws",
      "TypeError: Failed to execute 'takeJsObject' on 'JsValueContext': parameter 1 "
      "is not of type 'JsObject'.");
  e.expectEval("getDate() instanceof Date", "boolean", "true");
  e.expectEval(
      "checkProxyPrototype(new Proxy(class extends Foo{}, {})) === Foo", "boolean", "true");
  e.expectEval("checkProxyPrototype(new Proxy({}, { getPrototypeOf() { return Foo; } } )) === Foo",
      "boolean", "true");
  e.expectEval("checkProxyPrototype(new Proxy({}, { getPrototypeOf() { return String; } } )) "
               "=== Foo",
      "boolean", "false");
  e.expectEval("function f(val) { return this == globalThis && val === 1; }; callFunction(f);",
      "boolean", "true");
}

KJ_TEST("writeIntoUint8Array") {
  Evaluator<JsValueContext, JsValueIsolate> e(v8System);
  e.run([](Lock& js) {
    auto str = js.str("Hello, world! 👋"_kj);
    auto uint8Array = str.writeIntoUint8Array(js, SkipBailOutForTesting::YES);
    auto ptr = KJ_ASSERT_NONNULL(uint8Array).asArrayPtr();

    kj::byte check[] = {
      'H', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!', ' ', 0xf0, 0x9f, 0x91, 0x8b};

    KJ_EXPECT(ptr.size() == sizeof(check));

    KJ_EXPECT(ptr == kj::arrayPtr(check));
  });

  // Let's do a much larger string now.
  e.run([](Lock& js) {
    // Repeat the euro sign 1000 times.

    auto largeStr = kj::strArray(kj::repeat("€", 1000), "");
    auto str = js.str(largeStr);
    auto uint8Array = str.writeIntoUint8Array(js, SkipBailOutForTesting::YES);
    auto ptr = KJ_ASSERT_NONNULL(uint8Array).asArrayPtr();
    KJ_EXPECT(ptr.size() == 1000 * 3);  // Each euro sign is 3 bytes in UTF-8.

    kj::byte check[] = {0xe2, 0x82, 0xac};

    for (size_t i = 0; i < 1000; i++) {
      auto offset = i * 3;
      KJ_EXPECT(ptr.slice(offset, offset + 3) == kj::arrayPtr(check));
    }
  });

  e.run([](Lock& js) {
    // Create a string with 4095 ASCII chars + emoji (2 char16_t) + more content.
    // This ensures the lead surrogate is at position 4095 (last slot of first buffer chunk)
    // and trail surrogate is at position 4096 (would be in next chunk).
    kj::Vector<kj::String> parts;
    parts.add(kj::str(kj::repeat('A', 4095)));  // 4095 ASCII characters
    parts.add(kj::str("🎉"));                   // Emoji (U+1F389), which is a surrogate pair
    parts.add(kj::str("test"));                 // Some content after to continue processing

    auto testStr = kj::strArray(parts.asPtr(), "");
    auto str = js.str(testStr);
    auto uint8Array = str.writeIntoUint8Array(js, SkipBailOutForTesting::YES);
    auto ptr = KJ_ASSERT_NONNULL(uint8Array).asArrayPtr();

    // Expected: 4095 bytes (ASCII) + 4 bytes (emoji UTF-8) + 4 bytes ("test")
    KJ_EXPECT(ptr.size() == 4095 + 4 + 4);

    // Check the emoji is encoded correctly (U+1F389 -> F0 9F 8E 89 in UTF-8)
    kj::byte emojiCheck[] = {0xf0, 0x9f, 0x8e, 0x89};
    KJ_EXPECT(ptr.slice(4095, 4095 + 4) == kj::arrayPtr(emojiCheck));

    // Check the content after
    kj::byte testCheck[] = {'t', 'e', 's', 't'};
    KJ_EXPECT(ptr.slice(4099, 4099 + 4) == kj::arrayPtr(testCheck));
  });
}
}  // namespace
}  // namespace workerd::jsg::test
