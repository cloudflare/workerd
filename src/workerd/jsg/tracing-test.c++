// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});

class NumberBoxHolder: public Object {
  // An object that holds a NumberBox and implements GC visitation correctly.
  //
  // This differs from BoxBox (in jsg-test.h) in that this just holds the exact object you give
  // it, whereas BoxBox likes to create new objects.

public:
  explicit NumberBoxHolder(Ref<NumberBox> inner): inner(kj::mv(inner)) {}

  Ref<NumberBox> inner;

  static Ref<NumberBoxHolder> constructor(Ref<NumberBox> inner) {
    return jsg::alloc<NumberBoxHolder>(kj::mv(inner));
  }

  Ref<NumberBox> getInner() { return inner.addRef(); }

  JSG_RESOURCE_TYPE(NumberBoxHolder) {
    JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
  }

private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

class GcDetector: public jsg::Object {
  // Object which comes in pairs where one member of the pair can detect if the other has been
  // collected.

public:
  ~GcDetector() noexcept(false) {
    KJ_IF_SOME(s, sibling) s.sibling = kj::none;
  }

  kj::Maybe<GcDetector&> sibling;

  bool getSiblingCollected() { return sibling == kj::none; }

  bool touch() { return true; }

  JSG_RESOURCE_TYPE(GcDetector) {
    // NOTE: Using an instance property instead of a prototype property causes V8 to refuse to
    //   collect the wrapper during minor GCs, as it always thinks the wrapper is "modified".
    JSG_READONLY_PROTOTYPE_PROPERTY(siblingCollected, getSiblingCollected);
    JSG_METHOD(touch);
  }
};

class GcDetectorBox: public jsg::Object {
  // Contains a GcDetector. Useful for testing tracing scenarios.

public:
  jsg::Ref<GcDetector> inner = jsg::alloc<GcDetector>();

  jsg::Ref<GcDetector> getInner() { return inner.addRef(); }

  JSG_RESOURCE_TYPE(GcDetectorBox) {
    JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
  }

private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

class ValueBox: public jsg::Object {
  // Contains an arbitrary value.

public:
  ValueBox(jsg::Value inner): inner(kj::mv(inner)) {}

  static jsg::Ref<ValueBox> constructor(jsg::Value inner) {
    return jsg::alloc<ValueBox>(kj::mv(inner));
  }

  jsg::Value inner;

  jsg::Value getInner(jsg::Lock& lock) { return inner.addRef(lock); }

  JSG_RESOURCE_TYPE(ValueBox) {
    JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
  }

private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

struct TraceTestContext: public Object, public ContextGlobal {
  kj::Maybe<jsg::Ref<NumberBox>> strongRef;
  // A strong reference to a NumberBox which may be get and set.

  jsg::Ref<NumberBox> getStrongRef() {
    return KJ_REQUIRE_NONNULL(strongRef).addRef();
  }

  void setStrongRef(jsg::Ref<NumberBox> ref) {
    strongRef = kj::mv(ref);
  }

  kj::Array<jsg::Ref<GcDetector>> makeGcDetectorPair() {
    auto obj1 = jsg::alloc<GcDetector>();
    auto obj2 = jsg::alloc<GcDetector>();
    obj1->sibling = *obj2;
    obj2->sibling = *obj1;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  kj::Array<jsg::Ref<GcDetectorBox>> makeGcDetectorBoxPair() {
    auto obj1 = jsg::alloc<GcDetectorBox>();
    auto obj2 = jsg::alloc<GcDetectorBox>();
    obj1->inner->sibling = *obj2->inner;
    obj2->inner->sibling = *obj1->inner;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  void assert_(bool condition, jsg::Optional<kj::String> message) {
    JSG_ASSERT(condition, Error, message.orDefault(nullptr));
  }

  JSG_RESOURCE_TYPE(TraceTestContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(NumberBoxHolder);
    JSG_NESTED_TYPE(GcDetector);
    JSG_NESTED_TYPE(ValueBox);
    JSG_METHOD(makeGcDetectorPair);
    JSG_METHOD(makeGcDetectorBoxPair);
    JSG_METHOD_NAMED(assert, assert_);
    JSG_PROTOTYPE_PROPERTY(strongRef, getStrongRef, setStrongRef);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TraceTestIsolate, TraceTestContext, NumberBox,
                         NumberBoxHolder, GcDetector, GcDetectorBox, ValueBox);

KJ_TEST("GC collects objects when expected") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // Test that a full GC can collect native objects.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    a = null;
    gc();
    assert(b.siblingCollected, "full GC did not collect native objects");
  )", "undefined", "undefined");

  // Test that a full GC can collect native cyclic objects.
  e.expectEval(R"(
    let pair = makeGcDetectorBoxPair();
    let a = pair[0];
    let b = pair[1].inner;
    pair = null;
    a.inner.cycle = a;  // create cycle involving a jsg::Ref and a V8 native reference
    gc();
    assert(!b.siblingCollected);
    a = null;
    gc();
    assert(b.siblingCollected, "full GC did not collect cycles");
  )", "undefined", "undefined");

  // Test that minor GC can collect native objects.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    a = null;
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect native objects");
  )", "undefined", "undefined");

  // Test that minor GC does not collect native objects whose wrappers have been "modified".
  //
  // This verifies our assumptions about how V8's EmbedderRootHandler works.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    a.foo = 123;  // modify the wrapper
    a = null;
    gc({type: "minor"});
    assert(!b.siblingCollected, "minor GC collected modified native object");
  )", "undefined", "undefined");

  // Test that minor GC collects a native object contained in another native object.
  e.expectEval(R"(
    let pair = makeGcDetectorBoxPair();
    let a = pair[0];
    let b = pair[1].inner;
    pair = null;
    let inner = a.inner;
    // If I don't wrap `inner.touch()` in an IIFE then `inner` doesn't get collected (even with a
    // full GC). I guess when invoking a method on a native object, V8 ends up putting a handle on
    // the stack which doesn't get released until the end of the function? Weird but whatever.
    (() => {
      assert(inner.touch());  // make sure inner wrapper is initialized
    })();
    inner = null;
    a = null;
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect transitive native objects");
  )", "undefined", "undefined");

  // Test that minor GC can collect unreachable jsg::Value.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;

    // Without the IIFE here, a hidden reference gets left on the stack or something.
    (() => {
      a = new ValueBox(a);
    })();

    a = null;

    // We need two minor GC passes to fully collect the object. This is because the first GC pass
    // collects the `ValueBox`, thus destroying its `jsg::Value inner` member, but V8's GC doesn't
    // actually notice that this makes the inner object unreachable until a second pass.
    // TODO(perf): When V8 implements "unified young-generation", circle back and see if we can
    //   improved this.
    gc({type: "minor"});
    gc({type: "minor"});

    assert(b.siblingCollected, "minor GC did not collect jsg::Value");
  )", "undefined", "undefined");
}

KJ_TEST("TracedReference usage does not lead to crashes") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  e.expectEval(
      // Create an object holding another object.
      "let holder = new NumberBoxHolder(new NumberBox(123));\n"

      // Do a GC pass to make sure traced wrappers are allocated.
      "gc();\n"

      // Now put the NumberBox into the context's strongRef, and remove the holder. So now
      // the object is only reachable via a strong ref.
      "strongRef = holder.inner;\n"
      "holder = null;\n"

      // Invoke GC. Since the NumberBox is not reachable via tracing from any other object, its
      // tracedWrapper will not be marked and will therefore become invalid.
      "gc();\n"

      // Now create a new holder which holds the NumberBox.
      "holder = new NumberBoxHolder(strongRef);\n"

      // Invoke GC. The new holder will be traced, finding the NumberBox. It had better not crash
      // on the tracedWrapper having been collected!
      "gc();\n"

      // Verify the value is still there...
      "holder.inner.value", "number", "123");
}

}  // namespace
}  // namespace workerd::jsg::test
