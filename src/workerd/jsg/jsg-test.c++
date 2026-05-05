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
  e.expectEval("this instanceof TestContext", "boolean", "true");
}

// ========================================================================================

struct InheritContext: public ContextGlobalObject {
  struct Other: public Object {
    static jsg::Ref<Other> constructor(jsg::Lock& js) {
      return js.alloc<Other>();
    }
    JSG_RESOURCE_TYPE(Other) {}
  };

  Ref<NumberBox> newExtendedAsBase(jsg::Lock& js, double value, kj::String text) {
    return ExtendedNumberBox::constructor(js, value, kj::mv(text));
  }

  JSG_RESOURCE_TYPE(InheritContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(Other);
    JSG_NESTED_TYPE(ExtendedNumberBox);

    JSG_METHOD(newExtendedAsBase);
  }
};
JSG_DECLARE_ISOLATE_TYPE(
    InheritIsolate, InheritContext, NumberBox, InheritContext::Other, ExtendedNumberBox);

KJ_TEST("inheritance") {
  Evaluator<InheritContext, InheritIsolate> e(v8System);
  e.expectEval("var n = new ExtendedNumberBox(123, 'foo');\n"
               "n.increment();\n"
               "n.getValue()",
      "number", "124");

  e.expectEval("var n = new ExtendedNumberBox(123, 'foo');\n"
               "n.increment();\n"
               "n.value",
      "number", "124");

  e.expectEval("new ExtendedNumberBox(123, 'foo').getText()", "string", "foo");

  e.expectEval("var n = new ExtendedNumberBox(123, 'foo');\n"
               "n.setText('bar');\n"
               "n.text",
      "string", "bar");

  e.expectEval("var n = new ExtendedNumberBox(123, 'foo');\n"
               "n.text = 'bar';\n"
               "n.getText()",
      "string", "bar");

  e.expectEval("new ExtendedNumberBox(123, 'foo') instanceof NumberBox", "boolean", "true");

  e.expectEval("new ExtendedNumberBox(123, 'foo') instanceof ExtendedNumberBox", "boolean", "true");

  e.expectEval("new ExtendedNumberBox(123, 'foo') instanceof Other", "boolean", "false");

  e.expectEval("newExtendedAsBase(123, 'foo') instanceof NumberBox", "boolean", "true");

  e.expectEval("newExtendedAsBase(123, 'foo') instanceof ExtendedNumberBox", "boolean", "true");
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
  Ref<NumberBox> addAndReturnCopy(jsg::Lock& js, NumberBox& box, double value) {
    auto copy = js.alloc<NumberBox>(box.value);
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
  e.expectEval("var orig = new NumberBox(123);\n"
               "var result = addAndReturnCopy(orig, 321);\n"
               "[orig.value, result.value, orig == result].join(', ')",
      "string", "123, 444, false");

  // addAndReturnOwn() modifies the original object and returns it by identity.
  e.expectEval("var orig = new NumberBox(123);\n"
               "var result = addAndReturnOwn(orig, 321);\n"
               "[orig.value, result.value, orig == result].join(', ')",
      "string", "444, 444, true");
}

// ========================================================================================

struct ProtoContext: public ContextGlobalObject {
  ProtoContext(): contextProperty(kj::str("default-context-property-value")) {}

  kj::StringPtr getContextProperty() {
    return contextProperty;
  }
  void setContextProperty(kj::String s) {
    contextProperty = kj::mv(s);
  }

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

const auto kIllegalInvocation =
    "TypeError: Illegal invocation: function called with incorrect `this` reference. "
    "See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details."_kj;

KJ_TEST("can't invoke builtin methods with alternative 'this'") {
  Evaluator<ProtoContext, ProtoIsolate> e(v8System);
  e.expectEval("NumberBox.prototype.getValue.call(123)", "throws", kIllegalInvocation);
  e.expectEval("NumberBox.prototype.getValue.call(new BoxBox(new NumberBox(123), 123))", "throws",
      kIllegalInvocation);
  e.expectEval("getContextProperty.call(new NumberBox(123))", "throws", kIllegalInvocation);
}

KJ_TEST("can't use builtin as prototype") {
  Evaluator<ProtoContext, ProtoIsolate> e(v8System);
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = new NumberBox(123);\n"
               "new JsType().getValue()",
      "throws", kIllegalInvocation);
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = new ExtendedNumberBox(123, 'foo');\n"
               "new JsType().getValue()",
      "throws", kIllegalInvocation);
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = new NumberBox(123);\n"
               "new JsType().value",
      "number", "123");
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = new NumberBox(123);\n"
               "let t = new JsType();\n"
               "Reflect.get(JsType.prototype, 'value', t)\n",
      "number", "123");
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = new ExtendedNumberBox(123, 'foo');\n"
               "new JsType().value",
      "number", "123");
  e.expectEval("function JsType() {}\n"
               "JsType.prototype = this;\n"
               "new JsType().getContextProperty()",
      "throws", kIllegalInvocation);

  // For historical reasons, we allow using the global object as a prototype and accessing
  // properties through a derived object. Our accessor implementations for global object properties
  // ignore `this` and go directly to the singleton context object, so it doesn't matter.
  //
  // (Once upon a time, V8 supported a thing called an "AccessorSignature" which would handle the
  // type checking, but it didn't work correctly for the global object. V8 later removed
  // AccessorSignature entirely, forcing us to implement manual type checking. We could totally
  // make our manual type checking work correctly for global properties, but, again, it doesn't
  // really matter, and I'd rather not inadvertently break someone.)
  e.expectEval("function JsType() {}\n"
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
  e.expectEval("function charCodes(str) {"
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

      "string", "7835,803 7835,803 383,803,775 115,803,775 7785");
}

// ========================================================================================

KJ_TEST("Uncaught JsExceptionThrown reports stack") {
  auto exception =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { throw JsExceptionThrown(); }));
  KJ_ASSERT(
      exception.getDescription().startsWith("std::exception: Uncaught JsExceptionThrown\nstack: "),
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
    static Ref<MyCallable> constructor(jsg::Lock& js) {
      return js.alloc<MyCallable>();
    }

    bool foo() {
      return true;
    }

    JSG_RESOURCE_TYPE(MyCallable) {
      JSG_CALLABLE(foo);
      JSG_METHOD(foo);
    }
  };

  Ref<MyCallable> getCallable(jsg::Lock& js) {
    return js.alloc<MyCallable>();
  }

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
    static jsg::Ref<ProxyImpl> constructor(jsg::Lock& js) {
      return js.alloc<ProxyImpl>();
    }

    int getBar() {
      return 123;
    }

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
  e.expectEval("p = new ProxyImpl; p.hasOwnProperty('foo')", "boolean", "false");
  e.expectEval(
      "p = new ProxyImpl; Object.getOwnPropertyDescriptor(p, 'foo')", "undefined", "undefined");
  e.expectEval("p = new ProxyImpl; Reflect.has(p, 'bar')", "boolean", "true");
  e.expectEval("p = new ProxyImpl; Reflect.has(p, 'baz')", "boolean", "false");
  e.expectEval("p = new ProxyImpl; p.abc", "throws", "TypeError: boom");
}

// ========================================================================================
struct IsolateUuidContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(IsolateUuidContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(IsolateUuidIsolate, IsolateUuidContext);

KJ_TEST("External memory adjustment") {
  IsolateUuidIsolate isolate(v8System, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](IsolateUuidIsolate::Lock& lock) {
    // Creating an inner scope to check the case where the adjustment object does not outlive the isolate
    {
      // Creating with a specific amount works as expected
      auto adjuster = lock.getExternalMemoryAdjustment(100);
      KJ_ASSERT(adjuster.getAmount() == 100);

      // Adjusting up works as expected
      adjuster.adjust(10);
      KJ_ASSERT(adjuster.getAmount() == 110);

      // Adjusting down works as expected
      adjuster.adjust(-10);
      KJ_ASSERT(adjuster.getAmount() == 100);

      // Setting an explicit value just works
      adjuster.set(50);
      KJ_ASSERT(adjuster.getAmount() == 50);

      // Decrementing by more than the amount will throw an exception
      try {
        adjuster.adjust(-200);
      } catch (...) {
        auto exc = kj::getCaughtExceptionAsKj();
        KJ_ASSERT(exc.getDescription() ==
            "expected amount >= -static_cast<ssize_t>(this->amount) [-200 >= -50]; Memory usage may not be decreased below zero");
      }

      KJ_ASSERT(adjuster.getAmount() == 50);

      adjuster.set(100);
      auto adjuster2 = kj::mv(adjuster);
      KJ_ASSERT(adjuster2.getAmount() == 100);

      // Checking that the amount is zero after the adjuster is moved away would be nice to have,
      // but we should aim to avoid use-after-move entirely.
      // KJ_ASSERT(adjuster.getAmount() == 0);
    }
    // Note that we are not testing the actual effect on the isolate itself here.
    // While we have added a getExternalMemory() API to the isolate via a patch in
    // the internal repo, we have not added that patch to workerd so testing the
    // specific external memory reported by the isolate is possible but a bit
    // more cumbersome here.
  });
}

KJ_TEST("External memory adjustment - defered") {
  kj::Arc<const ExternalMemoryTarget> target;

  // A memory allocation that will outlive the isolate
  kj::Array<kj::byte> mem;

  {
    IsolateUuidIsolate isolate(v8System, kj::heap<IsolateObserver>());

    target = isolate.runInLockScope(
        [&](IsolateUuidIsolate::Lock& lock) { return lock.getExternalMemoryTarget(); });

    // Adjustment to memory while not holding lock will be applied later
    auto adjuster1 = target->getAdjustment(1000);
    KJ_ASSERT(adjuster1.getAmount() == 1000);
    KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 1000);

    {
      // This adjustment has no effect because the adjuster is destroyed before we take the lock again
      auto adjuster2 = target->getAdjustment(1000);
      KJ_ASSERT(adjuster2.getAmount() == 1000);
      KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 2000);
    }

    KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 1000);
    KJ_ASSERT(target->isIsolateAliveForTest());

    isolate.runInLockScope([&](IsolateUuidIsolate::Lock& lock) {
      // Once lock is taken, the amount is applied
      KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 0);

      // Adjustment made while holding lock applies immediately
      adjuster1.adjust(-500);
      KJ_ASSERT(adjuster1.getAmount() == 500);
      KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 0);
      KJ_ASSERT(target->isIsolateAliveForTest());
    });

    mem = isolate.runInLockScope([&](IsolateUuidIsolate::Lock& lock) {
      return kj::heapArray<kj::byte>(100).attach(target->getAdjustment(100));
    });
  }

  KJ_ASSERT(!target->isIsolateAliveForTest());

  // Delete the long-lived array, which will call the adjustment's destructor, to make sure it's safe.
  mem = nullptr;

  // Making an adjustment anyway won't do anything but also won't crash
  auto adjuster3 = target->getAdjustment(500);
  KJ_ASSERT(target->getPendingMemoryUpdateForTest() == 400);
}

KJ_TEST("Memory Allocation Error Propagation") {
  class MyAllocator final: public v8::ArrayBuffer::Allocator {
   public:
    void* Allocate(size_t length) override {
      return nullptr;
    }
    void* AllocateUninitialized(size_t length) override {
      return nullptr;
    }
    void Free(void* data, size_t length) override {}
    size_t MaxAllocationSize() const override {
      return 10;
    }
  };

  MyAllocator allocator;
  v8::Isolate::CreateParams createParams;
  createParams.constraints.ConfigureDefaults(10, 10);
  createParams.array_buffer_allocator = &allocator;
  IsolateUuidIsolate isolate(v8System, kj::heap<IsolateObserver>(), createParams);
  isolate.runInLockScope([&](IsolateUuidIsolate::Lock& lock) {
    KJ_EXPECT_THROW_MESSAGE(
        "Failed to allocate ArrayBuffer backing store", lock.allocBackingStore(100 * 1024));
  });
}

struct MpkContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(MpkContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(MpkIsolate, MpkContext);
KJ_TEST("MemoryProtectionKeyScope") {
  // In workerd, since V8_ENABLE_SANDBOX is not defined, this test is largely
  // a non-op, however, when v8 is built with V8_ENABLE_SANDBOX enabled and
  // the isolate has a memory protection key, this test will (eventually)
  // verify that the array buffer allocation is writable within the scope.
  // Essentially once backing stores are protected, and mpk's are enabled,
  // this shouldn't crash.
  MpkIsolate isolate(v8System, kj::heap<IsolateObserver>());
  std::shared_ptr<v8::BackingStore> store;
  auto mpkScope = isolate.runInLockScope([&](MpkIsolate::Lock& lock) {
    store = v8::ArrayBuffer::NewBackingStore(lock.v8Isolate, 10);
    return lock.getMemoryProtectionKeyScope();
  });
  bool called = false;
  // Now that we have our backing store, try writing to it outside the
  // isolate lock, but within the mpk scope. If mpk's are enabled writing
  // to the backing store without the mpk scope should segfault, but within
  // the mpk scope it should succeed.
  kj::ArrayPtr<kj::byte> bytes(static_cast<kj::byte*>(store->Data()), store->ByteLength());
  KJ_EXPECT(mpkScope.runWithKey([&] {
    called = true;
    bytes.fill(1);
    return 1;
  }) == 1);
  KJ_EXPECT(called);
}

}  // namespace

}  // namespace workerd::jsg::test
