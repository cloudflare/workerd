// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
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

}  // namespace
}  // namespace workerd::jsg::test
