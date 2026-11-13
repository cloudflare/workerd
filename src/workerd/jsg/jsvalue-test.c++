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
      "is not of type 'Object'.");
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

KJ_TEST("regression_AUTOVULN_CLOUDFLARE_WORKERD_143: deep proxy chain in getPrototype") {
  // Regression test for AUTOVULN-CLOUDFLARE-WORKERD-143: a deeply nested chain of
  // Proxy objects with no getPrototypeOf trap caused unbounded native recursion in
  // JsObject::getPrototype(), leading to SIGSEGV from stack overflow.
  // After the fix, getPrototype() iterates instead of recursing and throws a
  // RangeError when the depth limit is exceeded.
  Evaluator<JsValueContext, JsValueIsolate> e(v8System);
  e.expectEval(
      "let p = function(){}; for (let i = 0; i < 200000; i++) { p = new Proxy(p, {}); } try { checkProxyPrototype(p); 'no error'; } catch (e) { e.constructor.name + ': ' + e.message; }",
      "string", "RangeError: Maximum proxy chain length exceeded in getPrototype");
}

// Records the external string buffers the isolate asks for.
class LoggingExternalStringAllocator final: public ExternalStringAllocator {
 public:
  LoggingExternalStringAllocator(
      kj::Vector<kj::ArrayPtr<const char>>& allocated, kj::Vector<const void*>& freed)
      : allocated(allocated),
        freed(freed) {}

  void* allocate(size_t size) override {
    // It's a test so we don't mind using new and delete.
    void* ptr = operator new(size);
    allocated.add(kj::arrayPtr(static_cast<const char*>(ptr), size));
    return ptr;
  }
  void deallocate(void* ptr) override {
    freed.add(ptr);
    operator delete(ptr);
  }

 private:
  kj::Vector<kj::ArrayPtr<const char>>& allocated;
  kj::Vector<const void*>& freed;
};

KJ_TEST("copyToExternalString copies into the isolate's allocator and frees with the string") {
  kj::Vector<kj::ArrayPtr<const char>> allocated;
  kj::Vector<const void*> freed;
  static constexpr char TEXT[] = "copied text";
  static constexpr uint16_t WIDE_TEXT[] = {u'w', u'i', u'd', u'e', 0x20ac};
  auto text = kj::arrayPtr(TEXT, sizeof(TEXT) - 1);
  auto wideText = kj::arrayPtr(WIDE_TEXT);
  {
    JsValueIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>(),
        kj::heap<LoggingExternalStringAllocator>(allocated, freed));
    isolate.runInLockScope([&](JsValueIsolate::Lock& js) {
      auto oneByte = copyToExternalOneByteString(js, text);
      auto* oneByteResource = oneByte->GetExternalOneByteStringResource();
      KJ_ASSERT(oneByteResource != nullptr);
      KJ_EXPECT(oneByteResource->data() != TEXT);
      KJ_EXPECT(kj::arrayPtr(oneByteResource->data(), oneByteResource->length()) == text);

      auto twoByte = copyToExternalTwoByteString(js, wideText);
      auto* twoByteResource = twoByte->GetExternalStringResource();
      KJ_ASSERT(twoByteResource != nullptr);
      KJ_EXPECT(twoByteResource->data() != WIDE_TEXT);
      KJ_EXPECT(kj::arrayPtr(twoByteResource->data(), twoByteResource->length()) == wideText);

      KJ_ASSERT(allocated.size() == 2);
      KJ_EXPECT(allocated[0].begin() == oneByteResource->data());
      KJ_EXPECT(allocated[0].size() == text.size());
      KJ_EXPECT(allocated[1].begin() == reinterpret_cast<const char*>(twoByteResource->data()));
      KJ_EXPECT(allocated[1].size() == wideText.size() * sizeof(uint16_t));
      KJ_EXPECT(freed.size() == 0);

      // Copying nothing allocates nothing.
      KJ_EXPECT(copyToExternalOneByteString(js, text.first(0))->Length() == 0);
      KJ_EXPECT(allocated.size() == 2);
    });
  }
  // Disposing of the isolate disposes of the strings, and with them their buffers.
  KJ_ASSERT(freed.size() == 2);
  KJ_EXPECT(freed[0] == allocated[0].begin() || freed[0] == allocated[1].begin());
  KJ_EXPECT(freed[1] == allocated[0].begin() || freed[1] == allocated[1].begin());
  KJ_EXPECT(freed[0] != freed[1]);
}

}  // namespace
}  // namespace workerd::jsg::test
