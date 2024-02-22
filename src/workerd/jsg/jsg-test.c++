// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// Non-JSG types aren't GC-visitable.
static_assert(!isGcVisitable<int>());
static_assert(!isGcVisitable<kj::String>());

// Various reference types are.
static_assert(isGcVisitable<Ref<Object>>());
static_assert(isGcVisitable<kj::Maybe<Ref<Object>>>());
static_assert(isGcVisitable<Data>());
static_assert(isGcVisitable<V8Ref<v8::Object>>());

// Resource types are not directly visitable. Their visitForGc() is private. You should be visiting
// a Ref<T> pointing at them instead.
static_assert(!isGcVisitable<Object>());
static_assert(!isGcVisitable<NumberBox>());
static_assert(!isGcVisitable<BoxBox>());

// Any type that defines a public visitForGc() is visitable.
static_assert(isGcVisitable<TestStruct>());
static_assert(isGcVisitable<kj::Maybe<TestStruct>>());

// jsg::Lock is not acceptable as a coroutine param
static_assert(kj::_::isDisallowedInCoroutine<Lock>());
static_assert(kj::_::isDisallowedInCoroutine<Lock&>());
static_assert(kj::_::isDisallowedInCoroutine<Lock*>());

// ========================================================================================

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct TestContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

KJ_TEST("hello world") {
  Evaluator<TestContext, TestIsolate> e(v8System);
  e.expectEval("'Hello' + ', World!'", "string", "Hello, World!");
}

KJ_TEST("throw") {
  Evaluator<TestContext, TestIsolate> e(v8System);
  e.expectEval("throw new Error('some error message')", "throws", "Error: some error message");
}

KJ_TEST("context type is exposed in the global scope") {
  Evaluator<TestContext, TestIsolate> e(v8System);
  e.expectEval(
      "this instanceof TestContext",
      "boolean", "true"
  );
}

// ========================================================================================

struct InheritContext: public ContextGlobalObject {
  struct Other: public Object {
    JSG_RESOURCE_TYPE(Other) {}
  };

  Ref<NumberBox> newExtendedAsBase(double value, kj::String text) {
    return ExtendedNumberBox::constructor(value, kj::mv(text));
  }

  JSG_RESOURCE_TYPE(InheritContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(Other);
    JSG_NESTED_TYPE(ExtendedNumberBox);

    JSG_METHOD(newExtendedAsBase);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InheritIsolate, InheritContext, NumberBox,
    InheritContext::Other, ExtendedNumberBox);

KJ_TEST("inheritance") {
  Evaluator<InheritContext, InheritIsolate> e(v8System);
  e.expectEval(
      "var n = new ExtendedNumberBox(123, 'foo');\n"
      "n.increment();\n"
      "n.getValue()", "number", "124");

  e.expectEval(
      "var n = new ExtendedNumberBox(123, 'foo');\n"
      "n.increment();\n"
      "n.value", "number", "124");

  e.expectEval(
      "new ExtendedNumberBox(123, 'foo').getText()", "string", "foo");

  e.expectEval(
      "var n = new ExtendedNumberBox(123, 'foo');\n"
      "n.setText('bar');\n"
      "n.text", "string", "bar");

  e.expectEval(
      "var n = new ExtendedNumberBox(123, 'foo');\n"
      "n.text = 'bar';\n"
      "n.getText()", "string", "bar");

  e.expectEval(
      "new ExtendedNumberBox(123, 'foo') instanceof NumberBox", "boolean", "true");

  e.expectEval(
      "new ExtendedNumberBox(123, 'foo') instanceof ExtendedNumberBox", "boolean", "true");

  e.expectEval(
      "new ExtendedNumberBox(123, 'foo') instanceof Other", "boolean", "false");

  e.expectEval(
      "newExtendedAsBase(123, 'foo') instanceof NumberBox", "boolean", "true");

  e.expectEval(
      "newExtendedAsBase(123, 'foo') instanceof ExtendedNumberBox", "boolean", "true");
}

// ========================================================================================

struct Utf8Context: public ContextGlobalObject {
  bool callWithBmpUnicode(Lock& js, jsg::Function<bool(kj::StringPtr)> function) {
    return function(js, "中国网络");
  }
  bool callWithEmojiUnicode(Lock& js, jsg::Function<bool(kj::StringPtr)> function) {
    return function(js, "😺☁️☄️🐵");
  }
  JSG_RESOURCE_TYPE(Utf8Context) {
    JSG_METHOD(callWithBmpUnicode);
    JSG_METHOD(callWithEmojiUnicode);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Utf8Isolate, Utf8Context);

KJ_TEST("utf-8 scripts") {
  Evaluator<Utf8Context, Utf8Isolate> e(v8System);

  // BMP unicode.
  e.expectEval("'中国网络'", "string", "中国网络");

  // Emoji unicode (including non-BMP characters).
  e.expectEval("'😺☁️☄️🐵'", "string", "😺☁️☄️🐵");

  // Go the other way.
  e.expectEval("callWithBmpUnicode(str => str == '中国网络')", "boolean", "true");
  e.expectEval("callWithEmojiUnicode(str => str == '😺☁️☄️🐵')", "boolean", "true");
}

// ========================================================================================

struct RefContext: public ContextGlobalObject {
  Ref<NumberBox> addAndReturnCopy(NumberBox& box, double value) {
    auto copy = jsg::alloc<NumberBox>(box.value);
    copy->value += value;
    return copy;
  }
  Ref<NumberBox> addAndReturnOwn(Ref<NumberBox> box, double value) {
    box->value += value;
    return box;
  }

  JSG_RESOURCE_TYPE(RefContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(addAndReturnCopy);
    JSG_METHOD(addAndReturnOwn);
  }
};
JSG_DECLARE_ISOLATE_TYPE(RefIsolate, RefContext, NumberBox);

KJ_TEST("Ref") {
  Evaluator<RefContext, RefIsolate> e(v8System);
  // addAndReturnCopy() creates a new object and returns it.
  e.expectEval(
      "var orig = new NumberBox(123);\n"
      "var result = addAndReturnCopy(orig, 321);\n"
      "[orig.value, result.value, orig == result].join(', ')", "string", "123, 444, false");

  // addAndReturnOwn() modifies the original object and returns it by identity.
  e.expectEval(
      "var orig = new NumberBox(123);\n"
      "var result = addAndReturnOwn(orig, 321);\n"
      "[orig.value, result.value, orig == result].join(', ')", "string", "444, 444, true");
}

// ========================================================================================

struct ProtoContext: public ContextGlobalObject {
  ProtoContext(): contextProperty(kj::str("default-context-property-value")) {}

  kj::StringPtr getContextProperty() { return contextProperty; }
  void setContextProperty(kj::String s) { contextProperty = kj::mv(s); }

  JSG_RESOURCE_TYPE(ProtoContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(BoxBox);
    JSG_NESTED_TYPE(ExtendedNumberBox);
    JSG_METHOD(getContextProperty);
    JSG_METHOD(setContextProperty);
    JSG_INSTANCE_PROPERTY(contextProperty, getContextProperty, setContextProperty);
  }

private:
  kj::String contextProperty;
};
JSG_DECLARE_ISOLATE_TYPE(ProtoIsolate, ProtoContext, NumberBox, BoxBox, ExtendedNumberBox);

KJ_TEST("can't invoke builtin methods with alternative 'this'") {
  Evaluator<ProtoContext, ProtoIsolate> e(v8System);
  e.expectEval(
      "NumberBox.prototype.getValue.call(123)",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "NumberBox.prototype.getValue.call(new BoxBox(new NumberBox(123), 123))",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "getContextProperty.call(new NumberBox(123))",
      "throws", "TypeError: Illegal invocation");
}

KJ_TEST("can't use builtin as prototype") {
  Evaluator<ProtoContext, ProtoIsolate> e(v8System);
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = new NumberBox(123);\n"
      "new JsType().getValue()",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = new ExtendedNumberBox(123, 'foo');\n"
      "new JsType().getValue()",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = new NumberBox(123);\n"
      "new JsType().value",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = new ExtendedNumberBox(123, 'foo');\n"
      "new JsType().value",
      "throws", "TypeError: Illegal invocation");
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = this;\n"
      "new JsType().getContextProperty()",
      "throws", "TypeError: Illegal invocation");

  // For historical reasons, we allow using the global object as a prototype and accessing
  // properties through a derived object. Our accessor implementations for global object properties
  // ignore `this` and go directly to the singleton context object, so it doesn't matter.
  //
  // (Once upon a time, V8 supported a thing called an "AccessorSignature" which would handle the
  // type checking, but it didn't work correctly for the global object. V8 later removed
  // AccessorSignature entirely, forcing us to implement manual type checking. We could totally
  // make our manual type checking work correctly for global properties, but, again, it doesn't
  // really matter, and I'd rather not inadvertently break someone.)
  e.expectEval(
      "function JsType() {}\n"
      "JsType.prototype = this;\n"
      "new JsType().contextProperty",
      "string", "default-context-property-value");
}

// ========================================================================================

struct IcuContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(IcuContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(IcuIsolate, IcuContext);

KJ_TEST("ICU is properly initialized") {
  Evaluator<IcuContext, IcuIsolate> e(v8System);
  e.expectEval(
      "function charCodes(str) {"
      "  let result = [];\n"
      "  for (let i = 0; i < str.length; i++) {\n"
      "    result.push(str.charCodeAt(i));\n"
      "  }\n"
      "  return result.join(',');\n"
      "}"
      "[ charCodes('\u1E9B\u0323'),\n"
      "  charCodes('\u1E9B\u0323'.normalize('NFC')),\n"
      "  charCodes('\u1E9B\u0323'.normalize('NFD')),\n"
      "  charCodes('\u1E9B\u0323'.normalize('NFKD')),\n"
      "  charCodes('\u1E9B\u0323'.normalize('NFKC')) ].join(' ')",

      "string", "7835,803 7835,803 383,803,775 115,803,775 7785"
  );
}

// ========================================================================================

KJ_TEST("Uncaught JsExceptionThrown reports stack") {
  auto exception = KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() {
    throw JsExceptionThrown();
  }));
  KJ_ASSERT(exception.getDescription().startsWith(
                "std::exception: Uncaught JsExceptionThrown\nstack: "),
            exception.getDescription());
}

// TODO(test): Find some way to verify that C++ objects get garbage-collected as expected (hard to
//   test since GC technically does not guarantee that it will collect everything).

// ========================================================================================

struct LockLogContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(LockLogContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(LockLogIsolate, LockLogContext);

KJ_TEST("jsg::Lock logWarning") {
  LockLogIsolate isolate(v8System, kj::heap<IsolateObserver>());
  bool called = false;
  isolate.runInLockScope([&](LockLogIsolate::Lock& lock) {
    lock.setLoggerCallback([&called](jsg::Lock& js, auto message) {
      KJ_ASSERT(message == "Yes that happened"_kj);
      called = true;
    });
    lock.logWarning("Yes that happened"_kj);
    KJ_ASSERT(called);
  });
}

// ========================================================================================
// JSG_CALLABLE Test
struct CallableContext: public ContextGlobalObject {
  struct MyCallable: public Object {
  public:
    static Ref<MyCallable> constructor() { return alloc<MyCallable>(); }

    bool foo() { return true; }

    JSG_RESOURCE_TYPE(MyCallable) {
      JSG_CALLABLE(foo);
      JSG_METHOD(foo);
    }
  };

  Ref<MyCallable> getCallable() { return alloc<MyCallable>(); }

  JSG_RESOURCE_TYPE(CallableContext) {
    JSG_METHOD(getCallable);
    JSG_NESTED_TYPE(MyCallable);
  }
};
JSG_DECLARE_ISOLATE_TYPE(CallableIsolate, CallableContext, CallableContext::MyCallable);

KJ_TEST("Test JSG_CALLABLE") {
  Evaluator<CallableContext, CallableIsolate> e(v8System);

  e.expectEval("let obj = getCallable(); obj.foo();", "boolean", "true");
  e.expectEval("let obj = getCallable(); obj();", "boolean", "true");

  e.expectEval("let obj = new MyCallable(); obj();", "boolean", "true");

  // It's weird, but still accepted.
  e.expectEval("let obj = getCallable(); new obj();", "boolean", "true");
}

// ========================================================================================
struct InterceptContext: public ContextGlobalObject {
  struct ProxyImpl: public jsg::Object {
    static jsg::Ref<ProxyImpl> constructor() { return jsg::alloc<ProxyImpl>(); }

    int getBar() { return 123; }

    // JSG_WILDCARD_PROPERTY implementation
    kj::Maybe<kj::StringPtr> testGetNamed(jsg::Lock& js, kj::String name) {
      if (name == "foo") {
        return "bar"_kj;
      } else if (name == "abc") {
        JSG_FAIL_REQUIRE(TypeError, "boom");
      }
      return kj::none;
    }

    JSG_RESOURCE_TYPE(ProxyImpl) {
      JSG_READONLY_PROTOTYPE_PROPERTY(bar, getBar);
      JSG_WILDCARD_PROPERTY(testGetNamed);
    }
  };

  JSG_RESOURCE_TYPE(InterceptContext) {
    JSG_NESTED_TYPE(ProxyImpl);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InterceptIsolate, InterceptContext, InterceptContext::ProxyImpl);

KJ_TEST("Named interceptor") {
  Evaluator<InterceptContext, InterceptIsolate> e(v8System);
  e.expectEval("p = new ProxyImpl; p.bar", "number", "123");
  e.expectEval("p = new ProxyImpl; Reflect.has(p, 'foo')", "boolean", "true");
  e.expectEval("p = new ProxyImpl; Reflect.has(p, 'bar')", "boolean", "true");
  e.expectEval("p = new ProxyImpl; Reflect.has(p, 'baz')", "boolean", "false");
  e.expectEval("p = new ProxyImpl; p.abc", "throws", "TypeError: boom");
}

// ========================================================================================
struct IsolateUuidContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(IsolateUuidContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(IsolateUuidIsolate, IsolateUuidContext);

KJ_TEST("jsg::Lock getUuid") {
  IsolateUuidIsolate isolate(v8System, kj::heap<IsolateObserver>());
  bool called = false;
  isolate.runInLockScope([&](IsolateUuidIsolate::Lock& lock) {
    // Returns the same value
    KJ_ASSERT(lock.getUuid() == lock.getUuid());
    KJ_ASSERT(isolate.getUuid() == lock.getUuid());
    KJ_ASSERT(lock.getUuid().size() == 36);
    called = true;
  });
  KJ_ASSERT(called);
}

}  // namespace

}  // namespace workerd::jsg::test
