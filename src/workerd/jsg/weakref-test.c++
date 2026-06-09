// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});
class ContextGlobalObject: public Object, public ContextGlobal {};

struct WeakRefContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(WeakRefContext) {
    JSG_NESTED_TYPE(NumberBox);
  }
};
JSG_DECLARE_ISOLATE_TYPE(WeakRefIsolate, WeakRefContext, NumberBox);

// ========================================================================================
// jsg::WeakRef<T> tests

KJ_TEST("WeakRef: basic creation and access") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(42);
    auto weak = strong.getWeakRef(js);

    // Weak ref should be alive.
    KJ_ASSERT(weak.isAlive());

    // operator->() should work.
    KJ_ASSERT(weak->value == 42);

    // tryGet() should return the object.
    KJ_IF_SOME(ref, weak.tryGet()) {
      KJ_ASSERT(ref.value == 42);
    } else {
      KJ_FAIL_ASSERT("expected alive WeakRef");
    }
  });
}

KJ_TEST("WeakRef: tryAddRef promotes to strong Ref") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(7);
    auto weak = strong.getWeakRef(js);

    // Promote to strong reference.
    auto promoted = KJ_ASSERT_NONNULL(weak.tryAddRef(js));
    KJ_ASSERT(promoted->value == 7);

    // Both refs refer to the same object.
    KJ_ASSERT(&*strong == &*promoted);
  });
}

KJ_TEST("WeakRef: becomes invalid when all Refs dropped") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    WeakRef<NumberBox> weak(nullptr);

    {
      auto strong = js.alloc<NumberBox>(99);
      weak = strong.getWeakRef(js);
      KJ_ASSERT(weak.isAlive());
    }
    // strong is destroyed, Wrappable refcount hits 0, destructor invalidates anchor.
    KJ_ASSERT(!weak.isAlive());

    // tryGet returns none.
    KJ_ASSERT(weak.tryGet() == kj::none);

    // tryAddRef returns none.
    KJ_ASSERT(weak.tryAddRef(js) == kj::none);

    // operator->() throws.
    KJ_EXPECT_THROW_MESSAGE("invalidated", weak->value);

    // operation->() throws different message when weak itself is destroyed/moved
    auto weak2 = kj::mv(weak);
    KJ_EXPECT_THROW_MESSAGE("destroyed", weak->value);
  });
}

KJ_TEST("WeakRef: addRef creates independent weak ref") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(5);
    auto weak1 = strong.getWeakRef(js);
    auto weak2 = weak1.addRef(js);

    // Both alive.
    KJ_ASSERT(weak1.isAlive());
    KJ_ASSERT(weak2.isAlive());

    // Both refer to same object.
    auto& ref1 = KJ_ASSERT_NONNULL(weak1.tryGet());
    auto& ref2 = KJ_ASSERT_NONNULL(weak2.tryGet());
    KJ_ASSERT(&ref1 == &ref2);
  });
}

KJ_TEST("WeakRef: null-constructed is not alive") {
  WeakRef<NumberBox> weak(nullptr);
  KJ_ASSERT(!weak.isAlive());
  KJ_ASSERT(weak.tryGet() == kj::none);
}

KJ_TEST("WeakRef: move semantics") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(3);
    auto weak1 = strong.getWeakRef(js);
    auto weak2 = kj::mv(weak1);

    // weak2 should be alive, weak1 should be null.
    KJ_ASSERT(weak2.isAlive());
    KJ_ASSERT(!weak1.isAlive());
    KJ_ASSERT(weak2->value == 3);
  });
}

KJ_TEST("WeakRef: promote keeps object alive") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    WeakRef<NumberBox> weak(nullptr);
    kj::Maybe<Ref<NumberBox>> maybePromoted;

    {
      auto strong = js.alloc<NumberBox>(11);
      weak = strong.getWeakRef(js);
      maybePromoted = weak.tryAddRef(js);
      // strong goes out of scope, but promoted keeps object alive.
    }

    // Object is still alive because promoted ref exists.
    KJ_ASSERT(weak.isAlive());

    auto& promoted = KJ_ASSERT_NONNULL(maybePromoted);
    KJ_ASSERT(promoted->value == 11);
  });
}

KJ_TEST("WeakRef: drop out of lock") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  kj::Maybe<jsg::WeakRef<NumberBox>> weak;
  e.run([&weak](Lock& js) {
    auto strong = js.alloc<NumberBox>(11);
    weak = strong.getWeakRef(js);
    KJ_ASSERT(KJ_ASSERT_NONNULL(weak).isAlive());
  });

  // We are now outside the isolate lock. The
  // strong object is not alive,
  KJ_ASSERT(!KJ_ASSERT_NONNULL(weak).isAlive());
  weak = kj::none;

  e.run([](Lock& js) {
    // The weak should be destroyed finally when
    // we entered this lock. Don't crash!
  });
}

KJ_TEST("WeakRef: drop out of lock (drop in any order)") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  kj::Maybe<jsg::WeakRef<NumberBox>> weak;
  kj::Maybe<jsg::Ref<NumberBox>> strong;
  e.run([&](Lock& js) {
    strong = js.alloc<NumberBox>(11);
    weak = KJ_ASSERT_NONNULL(strong).getWeakRef(js);
    KJ_ASSERT(KJ_ASSERT_NONNULL(weak).isAlive());
  });

  // The order in which the items are dropped outside of the
  // isolate lock determines the order in which they are
  // added to the deferred struction queue.
  weak = kj::none;
  strong = kj::none;

  e.run([](Lock& js) {
    // The weak should be destroyed finally when
    // we entered this lock. Don't crash!
  });
}

KJ_TEST("WeakRef: drop out of lock (drop in any order 2)") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  kj::Maybe<jsg::WeakRef<NumberBox>> weak;
  kj::Maybe<jsg::Ref<NumberBox>> strong;
  e.run([&](Lock& js) {
    strong = js.alloc<NumberBox>(11);
    weak = KJ_ASSERT_NONNULL(strong).getWeakRef(js);
    KJ_ASSERT(KJ_ASSERT_NONNULL(weak).isAlive());
  });

  // The order in which the items are dropped outside of the
  // isolate lock determines the order in which they are
  // added to the deferred struction queue.
  strong = kj::none;
  weak = kj::none;

  e.run([](Lock& js) {
    // The weak should be destroyed finally when
    // we entered this lock. Don't crash!
  });
}

class NumberBox2: public NumberBox {
 public:
  using NumberBox::NumberBox;
  JSG_RESOURCE_TYPE(NumberBox2) {
    JSG_INHERIT(NumberBox);
  }
};

KJ_TEST("Moving WeakRefs") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);

  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(123);
    auto weak1 = strong.getWeakRef(js);
    auto weak2 = kj::mv(weak1);
    KJ_ASSERT(weak2.isAlive());
    KJ_ASSERT(!weak1.isAlive());

    auto strong2 = js.alloc<NumberBox2>(456);
    auto weak3 = strong2.getWeakRef(js);
    jsg::WeakRef<NumberBox> weak4 = kj::mv(weak3);
    jsg::WeakRef<NumberBox> weak5(strong2.getWeakRef(js));
    KJ_ASSERT(KJ_ASSERT_NONNULL(weak4.tryGet()).value == 456);
    KJ_ASSERT(KJ_ASSERT_NONNULL(weak5.tryGet()).value == 456);
  });
}

KJ_TEST("Getting weakref from self") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);

  e.run([](Lock& js) {
    auto strong = js.alloc<NumberBox>(123);
    // Uses JSG_THIS_WEAK internally
    auto weak = strong->getWeakRefToSelf(js);
    KJ_ASSERT(weak.isAlive());
    auto strong2 = KJ_ASSERT_NONNULL(weak.tryAddRef(js));
    KJ_ASSERT(strong.get() == strong2.get());
  });
}

// ========================================================================================
// jsg::WeakV8Ref<T> tests

KJ_TEST("WeakV8Ref: basic creation and access") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    // Create a V8 value and a weak ref to it.
    auto strong = js.v8Ref(v8Str(js.v8Isolate, "hello"_kj));
    auto weak = strong.getWeakRef(js);

    KJ_ASSERT(weak.isAlive());

    auto local = KJ_ASSERT_NONNULL(weak.tryGetHandle(js.v8Isolate));
    v8::String::Utf8Value utf8(js.v8Isolate, local);
    KJ_ASSERT(kj::StringPtr(*utf8, utf8.length()) == "hello");
  });
}

KJ_TEST("WeakV8Ref: tryAddRef promotes to strong V8Ref") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.v8Ref(v8Str(js.v8Isolate, "world"_kj));
    auto weak = strong.getWeakRef(js);

    auto promoted = KJ_ASSERT_NONNULL(weak.tryAddRef(js.v8Isolate));
    auto local = promoted.getHandle(js.v8Isolate);
    v8::String::Utf8Value utf8(js.v8Isolate, local);
    KJ_ASSERT(kj::StringPtr(*utf8, utf8.length()) == "world");
  });
}

KJ_TEST("WeakV8Ref: null-constructed is not alive") {
  WeakV8Ref<v8::String> weak(nullptr);
  KJ_ASSERT(!weak.isAlive());
}

KJ_TEST("WeakV8Ref: move semantics") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto strong = js.v8Ref(v8Str(js.v8Isolate, "test"_kj));
    auto weak1 = strong.getWeakRef(js);
    auto weak2 = kj::mv(weak1);

    KJ_ASSERT(weak2.isAlive());
    KJ_ASSERT(!weak1.isAlive());
  });
}

KJ_TEST("WeakV8Ref: not alive after drop") {
  setPredictableModeForTest();
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    // A nested handle scope is required to ensure that the object is collected
    // and not being held alive by the outer handle scope.
    auto weak = js.withinHandleScope([&] {
      auto strong = js.v8Ref(v8::Object::New(js.v8Isolate));
      auto weak = strong.getWeakRef(js);
      KJ_ASSERT(weak.isAlive());
      return kj::mv(weak);
    });
    js.requestGcForTesting();
    KJ_ASSERT(!weak.isAlive());
  });
}

// ========================================================================================
// jsg::WeakJsRef<T> tests

KJ_TEST("WeakJsRef: basic creation and access") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto obj = js.obj();
    JsRef<JsObject> strong(js, obj);
    auto weak = strong.getWeakRef(js);

    KJ_ASSERT(weak.isAlive());

    auto handle = KJ_ASSERT_NONNULL(weak.tryGetHandle(js));
    // Should be the same object.
    KJ_ASSERT(handle == obj);
  });
}

KJ_TEST("WeakJsRef: tryAddRef promotes to strong JsRef") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto str = js.str("test"_kj);
    JsRef<JsString> strong(js, str);
    auto weak = strong.getWeakRef(js);

    auto promoted = KJ_ASSERT_NONNULL(weak.tryAddRef(js));
    auto handle = promoted.getHandle(js);
    KJ_ASSERT(handle == str);
  });
}

KJ_TEST("WeakJsRef: null-constructed is not alive") {
  WeakJsRef<JsValue> weak(nullptr);
  KJ_ASSERT(!weak.isAlive());
}

KJ_TEST("WeakJsRef: move semantics") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    auto obj = js.obj();
    JsRef<JsObject> strong(js, obj);
    auto weak1 = strong.getWeakRef(js);
    auto weak2 = kj::mv(weak1);

    KJ_ASSERT(weak2.isAlive());
    KJ_ASSERT(!weak1.isAlive());
  });
}

KJ_TEST("WeakJsRef: getHandle asserts when dead") {
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    WeakJsRef<JsObject> weak(nullptr);
    KJ_EXPECT_THROW_MESSAGE("collected", weak.getHandle(js));
  });
}

KJ_TEST("WeakJsRef: not alive after drop") {
  setPredictableModeForTest();
  Evaluator<WeakRefContext, WeakRefIsolate> e(v8System);
  e.run([](Lock& js) {
    // A nested handle scope is required to ensure that the object is collected
    // and not being held alive by the outer handle scope.
    auto weak = js.withinHandleScope([&] {
      auto obj = js.obj();
      auto weak = obj.getWeakRef(js);
      KJ_ASSERT(weak.isAlive());
      return kj::mv(weak);
    });
    js.requestGcForTesting();
    KJ_ASSERT(!weak.isAlive());
  });
}

}  // namespace
}  // namespace workerd::jsg::test
