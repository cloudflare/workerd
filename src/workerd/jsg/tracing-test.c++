// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <workerd/util/autogate.h>

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

  static Ref<NumberBoxHolder> constructor(jsg::Lock& js, Ref<NumberBox> inner) {
    return js.alloc<NumberBoxHolder>(kj::mv(inner));
  }

  Ref<NumberBox> getInner() {
    return inner.addRef();
  }

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

  bool getSiblingCollected() {
    return sibling == kj::none;
  }

  bool touch() {
    return true;
  }

  JSG_RESOURCE_TYPE(GcDetector) {
    JSG_READONLY_PROTOTYPE_PROPERTY(siblingCollected, getSiblingCollected);
    JSG_METHOD(touch);
  }
};

// A GcDetector with properties on its instance template, as ExecutionContext has. V8 installs
// them on each wrapper as native data properties, so no wrapper is ever on its constructor's
// initial map.
class InstancePropertyGcDetector: public jsg::Object {
 public:
  ~InstancePropertyGcDetector() noexcept(false) {
    KJ_IF_SOME(s, sibling) s.sibling = kj::none;
  }

  kj::Maybe<InstancePropertyGcDetector&> sibling;

  bool getSiblingCollected() {
    return sibling == kj::none;
  }

  int getEager() {
    return 1;
  }

  int getLazy() {
    return 2;
  }

  int value = 0;

  int getValue() {
    return value;
  }

  void setValue(int newValue) {
    value = newValue;
  }

  JSG_RESOURCE_TYPE(InstancePropertyGcDetector) {
    JSG_READONLY_PROTOTYPE_PROPERTY(siblingCollected, getSiblingCollected);
    JSG_READONLY_INSTANCE_PROPERTY(eager, getEager);
    JSG_LAZY_INSTANCE_PROPERTY(lazy, getLazy);
    JSG_INSTANCE_PROPERTY(value, getValue, setValue);
  }
};

// Adds an instance property of its own to those it inherits, as Request does to Body's when its
// compatibility date predates workers_api_getters_setters_on_prototype.
class InheritedInstancePropertyGcDetector: public InstancePropertyGcDetector {
 public:
  int getChildEager() {
    return 3;
  }

  JSG_RESOURCE_TYPE(InheritedInstancePropertyGcDetector) {
    JSG_INHERIT(InstancePropertyGcDetector);
    JSG_READONLY_INSTANCE_PROPERTY(childEager, getChildEager);
  }
};

// Redefines an instance property it inherits. V8 installs only one property under the name, so
// the wrapper never holds every accessor in its templates.
class ShadowingInstancePropertyGcDetector: public InstancePropertyGcDetector {
 public:
  int getShadowedEager() {
    return 4;
  }

  JSG_RESOURCE_TYPE(ShadowingInstancePropertyGcDetector) {
    JSG_INHERIT(InstancePropertyGcDetector);
    JSG_READONLY_INSTANCE_PROPERTY(eager, getShadowedEager);
  }
};

// Holds an InstancePropertyGcDetector through a traced reference, so the detector outlives its
// wrapper if V8 drops the wrapper, and a later read of `inner` wraps it again.
class InstancePropertyGcDetectorBox: public jsg::Object {
 public:
  explicit InstancePropertyGcDetectorBox(jsg::Lock& js)
      : inner(js.alloc<InstancePropertyGcDetector>()) {}

  jsg::Ref<InstancePropertyGcDetector> inner;

  jsg::Ref<InstancePropertyGcDetector> getInner() {
    return inner.addRef();
  }

  bool getInnerWrapped(jsg::Lock& js) {
    // Wrappable::tryGetHandle() is out of reach of a jsg::Object subclass; this returns its
    // result, or an empty handle when there is no wrapper.
    return !inner->jsgGetMemoryInfoWrapperObject(js.v8Isolate).IsEmpty();
  }

  JSG_RESOURCE_TYPE(InstancePropertyGcDetectorBox) {
    JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
    JSG_READONLY_PROTOTYPE_PROPERTY(innerWrapped, getInnerWrapped);
  }

 private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

class GcDetectorBox: public jsg::Object {
  // Contains a GcDetector. Useful for testing tracing scenarios.

 public:
  GcDetectorBox(jsg::Lock& js): inner(js.alloc<GcDetector>()) {}
  jsg::Ref<GcDetector> inner;

  jsg::Ref<GcDetector> getInner() {
    return inner.addRef();
  }

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

  static jsg::Ref<ValueBox> constructor(jsg::Lock& js, jsg::Value inner) {
    return js.alloc<ValueBox>(kj::mv(inner));
  }

  jsg::Value inner;

  jsg::Value getInner(jsg::Lock& lock) {
    return inner.addRef(lock);
  }

  JSG_RESOURCE_TYPE(ValueBox) {
    JSG_READONLY_PROTOTYPE_PROPERTY(inner, getInner);
  }

 private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

// A heap object whose handles the queue holding it visits.
struct QueuedItem {
  kj::Maybe<jsg::Value> value;
  kj::Maybe<jsg::Ref<GcDetector>> detector;

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(value, detector);
  }
};

class ItemQueue: public jsg::Object {
 public:
  static jsg::Ref<ItemQueue> constructor(jsg::Lock& js) {
    return js.alloc<ItemQueue>();
  }

  void pushValue(jsg::Value value) {
    items.add(TracedOwn(kj::heap(QueuedItem{.value = kj::mv(value)})));
  }

  void pushDetector(jsg::Ref<GcDetector> detector) {
    items.add(TracedOwn(kj::heap(QueuedItem{.detector = kj::mv(detector)})));
  }

  kj::Vector<TracedOwn<QueuedItem>> items;

  JSG_RESOURCE_TYPE(ItemQueue) {
    JSG_METHOD(pushValue);
    JSG_METHOD(pushDetector);
  }

 private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visitAll(items);
  }
};

struct TraceTestContext: public Object, public ContextGlobal {
  // An item taken out of an ItemQueue. Nothing visits it.
  kj::Maybe<kj::Own<QueuedItem>> taken;

  void take(jsg::Ref<ItemQueue> queue) {
    auto item = kj::mv(queue->items.back());
    queue->items.removeLast();
    taken = item.release();
  }

  void putBack(jsg::Ref<ItemQueue> queue) {
    queue->items.add(TracedOwn(KJ_ASSERT_NONNULL(kj::mv(taken))));
    taken = kj::none;
  }

  kj::Maybe<jsg::Ref<NumberBox>> strongRef;
  // A strong reference to a NumberBox which may be get and set.

  jsg::Ref<NumberBox> getStrongRef() {
    return KJ_REQUIRE_NONNULL(strongRef).addRef();
  }

  void setStrongRef(jsg::Ref<NumberBox> ref) {
    strongRef = kj::mv(ref);
  }

  kj::Array<jsg::Ref<GcDetector>> makeGcDetectorPair(jsg::Lock& js) {
    auto obj1 = js.alloc<GcDetector>();
    auto obj2 = js.alloc<GcDetector>();
    obj1->sibling = *obj2;
    obj2->sibling = *obj1;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  template <typename Detector>
  static kj::Array<jsg::Ref<InstancePropertyGcDetector>> makeDetectorPair(jsg::Lock& js) {
    jsg::Ref<InstancePropertyGcDetector> obj1 = js.alloc<Detector>();
    jsg::Ref<InstancePropertyGcDetector> obj2 = js.alloc<Detector>();
    obj1->sibling = *obj2;
    obj2->sibling = *obj1;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  kj::Array<jsg::Ref<InstancePropertyGcDetector>> makeInstancePropertyGcDetectorPair(
      jsg::Lock& js) {
    return makeDetectorPair<InstancePropertyGcDetector>(js);
  }

  kj::Array<jsg::Ref<InstancePropertyGcDetector>> makeInheritedInstancePropertyGcDetectorPair(
      jsg::Lock& js) {
    return makeDetectorPair<InheritedInstancePropertyGcDetector>(js);
  }

  kj::Array<jsg::Ref<InstancePropertyGcDetector>> makeShadowingInstancePropertyGcDetectorPair(
      jsg::Lock& js) {
    return makeDetectorPair<ShadowingInstancePropertyGcDetector>(js);
  }

  jsg::Ref<InstancePropertyGcDetectorBox> makeInstancePropertyGcDetectorBox(jsg::Lock& js) {
    return js.alloc<InstancePropertyGcDetectorBox>(js);
  }

  kj::Array<jsg::Ref<GcDetectorBox>> makeGcDetectorBoxPair(jsg::Lock& js) {
    auto obj1 = js.alloc<GcDetectorBox>(js);
    auto obj2 = js.alloc<GcDetectorBox>(js);
    obj1->inner->sibling = *obj2->inner;
    obj2->inner->sibling = *obj1->inner;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  // Hides `detector` in an opaque wrapper, the kind that jsg::Promise uses to carry C++ values.
  jsg::JsValue wrapOpaquely(jsg::Lock& js, jsg::Ref<GcDetector> detector) {
    return jsg::JsValue(wrapOpaque(js.v8Context(), kj::mv(detector)));
  }

  void assert_(bool condition, jsg::Optional<kj::String> message) {
    JSG_ASSERT(condition, Error, message.orDefault(nullptr));
  }

  JSG_RESOURCE_TYPE(TraceTestContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(NumberBoxHolder);
    JSG_NESTED_TYPE(GcDetector);
    JSG_NESTED_TYPE(ValueBox);
    JSG_NESTED_TYPE(ItemQueue);
    JSG_METHOD(take);
    JSG_METHOD(putBack);
    JSG_METHOD(makeGcDetectorPair);
    JSG_METHOD(makeGcDetectorBoxPair);
    JSG_METHOD(makeInstancePropertyGcDetectorPair);
    JSG_METHOD(makeInheritedInstancePropertyGcDetectorPair);
    JSG_METHOD(makeShadowingInstancePropertyGcDetectorPair);
    JSG_METHOD(makeInstancePropertyGcDetectorBox);
    JSG_METHOD(wrapOpaquely);
    JSG_METHOD_NAMED(assert, assert_);
    JSG_PROTOTYPE_PROPERTY(strongRef, getStrongRef, setStrongRef);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TraceTestIsolate,
    TraceTestContext,
    NumberBox,
    NumberBoxHolder,
    GcDetector,
    InstancePropertyGcDetector,
    InheritedInstancePropertyGcDetector,
    ShadowingInstancePropertyGcDetector,
    InstancePropertyGcDetectorBox,
    GcDetectorBox,
    ValueBox,
    ItemQueue);

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
  )",
      "undefined", "undefined");

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
  )",
      "undefined", "undefined");

  // Test that minor GC can collect native objects.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    a = null;
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect native objects");
  )",
      "undefined", "undefined");

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
  )",
      "undefined", "undefined");

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
  )",
      "undefined", "undefined");

  // Test that minor GC collects an opaque wrapper and the native object it holds.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let b = pair[1];
    (() => {
      wrapOpaquely(pair[0]);
    })();
    pair = null;
    // As with ValueBox below, the first pass drops the opaque wrapper, which releases its strong
    // reference to the detector, and the second drops the detector's own wrapper.
    gc({type: "minor"});
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect an opaque wrapper");
  )",
      "undefined", "undefined");

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
  )",
      "undefined", "undefined");
}

// Makes a pair of detectors with `factory`, runs `change` on the first, drops it and runs a minor
// GC, then expects the first to have been collected or not, as `collected` says.
void expectMinorGcCollects(Evaluator<TraceTestContext, TraceTestIsolate>& e,
    bool collected,
    kj::StringPtr change,
    kj::StringPtr factory = "makeInstancePropertyGcDetectorPair"_kj) {
  KJ_CONTEXT(change, factory);
  // `change` runs in a function that takes the wrapper as an argument, so no variable of the
  // script's still holds it at the GC.
  e.expectEval(kj::str("let pair = ", factory,
                   R"(();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    ((a) => {
      )",
                   change, R"(
    })(a);
    a = null;
    gc({type: "minor"});
    b.siblingCollected;
  )"),
      "boolean", collected ? "true" : "false");
}

KJ_TEST("minor GC collects wrappers with instance properties") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // The instance properties alone don't make the wrapper "modified".
  expectMinorGcCollects(e, true, "");

  // Neither does reading an eager instance property.
  expectMinorGcCollects(e, true, "assert(a.eager === 1);");

  // Nor writing one with a setter, which stores the value in the C++ object.
  expectMinorGcCollects(e, true, "a.value = 7; assert(a.value === 7);");

  // Nor a write to a readonly one, which sloppy mode ignores.
  expectMinorGcCollects(e, true, "a.eager = 5; assert(a.eager === 1);");

  // A type's wrappers hold the instance properties of its parent types too.
  expectMinorGcCollects(e, true, "", "makeInheritedInstancePropertyGcDetectorPair");
  expectMinorGcCollects(e, true, "assert(a.eager === 1 && a.childEager === 3);",
      "makeInheritedInstancePropertyGcDetectorPair");
}

KJ_TEST("minor GC keeps wrappers that hold more than their instance properties") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // Reading a lazy instance property stores its value on the wrapper.
  expectMinorGcCollects(e, false, "assert(a.lazy === 2);");

  // So does writing one, which replaces it with a data property.
  expectMinorGcCollects(e, false, "a.lazy = 3;");

  // Each of these changes the wrapper's map in a way a fresh instance can't have.
  expectMinorGcCollects(e, false, "delete a.lazy;");
  expectMinorGcCollects(e, false, "a.foo = 1;");
  expectMinorGcCollects(e, false, "a[0] = 1;");
  expectMinorGcCollects(e, false, "Object.setPrototypeOf(a, {});");
  expectMinorGcCollects(e, false, "Object.preventExtensions(a);");
  expectMinorGcCollects(e, false, "Object.seal(a);");
  expectMinorGcCollects(e, false, "Object.freeze(a);");
  expectMinorGcCollects(e, false, "Object.defineProperty(a, 'eager', {enumerable: false});");

  // A private field, added through a base class constructor that returns the wrapper.
  expectMinorGcCollects(e, false, R"(
      class Base { constructor(o) { return o; } }
      class WithField extends Base { #x = 1; }
      new WithField(a);
  )");

  // Using the wrapper as a WeakMap key gives it an identity hash.
  expectMinorGcCollects(e, false, "globalThis.keys = new WeakMap(); keys.set(a, 1);");

  // A type that redefines an inherited instance property has one property fewer than its
  // templates have accessors, so its wrappers are never taken for fresh instances.
  expectMinorGcCollects(e, false, "", "makeShadowingInstancePropertyGcDetectorPair");
}

KJ_TEST("minor GC drops the wrapper of a live object with instance properties") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // The box keeps the detector alive through a traced reference, so a minor GC can drop the
  // detector's wrapper, and reading `box.inner` again wraps it anew.
  e.expectEval(R"(
    let box = makeInstancePropertyGcDetectorBox();
    ((d) => {
      d.value = 7;
      assert(d.eager === 1);
    })(box.inner);
    assert(box.innerWrapped);
    gc({type: "minor"});
    assert(!box.innerWrapped, "minor GC did not drop the wrapper of a traced object");
    ((d) => {
      assert(d.value === 7, "the new wrapper does not reach the C++ object's state");
      assert(d.eager === 1);
      assert(d.lazy === 2);
    })(box.inner);
    assert(box.innerWrapped);
  )",
      "undefined", "undefined");
}

KJ_TEST("minor GC keeps the wrapper a WeakRef points to") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    lock.withinHandleScope([&] {
      // Both scripts run in one context, which has the WeakRef global.
      auto context = lock.newContext<TraceTestContext>(NewContextOptions{.enableWeakRef = true})
                         .getHandle(lock);
      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](Lock& js) {
        auto run = [&](kj::StringPtr code) {
          auto script = check(v8::Script::Compile(js.v8Context(), v8Str(js.v8Isolate, code)));
          return check(script->Run(js.v8Context()));
        };
        run(R"(
          globalThis.box = makeInstancePropertyGcDetectorBox();
          globalThis.ref = new WeakRef(box.inner);
        )");
        // A new WeakRef keeps its target alive until the job that made it ends.
        js.v8Isolate->ClearKeptObjects();
        // If V8 dropped the wrapper, the WeakRef would come back empty while the detector lives,
        // and `box.inner` would be a new wrapper.
        auto result = run(R"(
          gc({type: "minor"});
          ref.deref() === box.inner;
        )");
        KJ_EXPECT(result->IsTrue(), "minor GC dropped the wrapper a WeakRef points to");
      });
    });
  });
}

KJ_TEST("TracedOwn keeps an object's handles alive after it leaves the owner that visited it") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);
  util::Autogate::initAutogateForTest({util::AutogateKey::GC_COMPLETE_TRACING});
  KJ_DEFER(util::Autogate::deinitAutogate());

  // A jsg::Value taken out of a traced queue stays alive after the queue is collected.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let b = pair[1];
    (() => {
      const queue = new ItemQueue();
      queue.pushValue(pair[0]);
      pair = null;
      gc();  // traces the queued value
      take(queue);
    })();
    gc();
    assert(!b.siblingCollected, "a value taken out of a traced queue was collected");
  )",
      "undefined", "undefined");

  // So does a jsg::Ref, and a different queue can then visit it.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let b = pair[1];
    (() => {
      const queue = new ItemQueue();
      queue.pushDetector(pair[0]);
      pair = null;
      gc();
      take(queue);
    })();
    gc();
    assert(!b.siblingCollected, "a Ref taken out of a traced queue was collected");
    (() => {
      const other = new ItemQueue();
      putBack(other);
      gc();
      assert(!b.siblingCollected, "a Ref put in a live queue was collected");
    })();
    gc();
    assert(b.siblingCollected, "a Ref left in a dead queue was not collected");
  )",
      "undefined", "undefined");

  // A value still in the queue is collected with the queue, even when it refers to the queue.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let b = pair[1];
    (() => {
      const queue = new ItemQueue();
      pair[0].queue = queue;
      queue.pushValue(pair[0]);
      pair = null;
      gc();  // A handle added to an object that has a wrapper stays strong until a trace visits it.
    })();
    gc();
    assert(b.siblingCollected, "a queue holding a cycle through its items was not collected");
  )",
      "undefined", "undefined");
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
      "holder.inner.value",
      "number", "123");
}

KJ_TEST("allocating short-lived wrappers eventually triggers a major GC") {
  // Every wrapper takes entries in V8's CppHeap pointer table, which only a major GC frees. A
  // loop of wrappers that die young only triggers minor GCs by itself, so a major GC has to come
  // from the wrappers' cppgc allocations being reported to V8. If they are not, the table fills
  // and the process aborts with "ExternalEntityTable::AllocateEntry: allocation failed".
  uint majorGcCount = 0;
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // A fresh isolate starts with a large global allocation limit. A full GC sets it from the live
  // heap, which is nearly empty, so the loop below crosses it quickly.
  e.expectEval("gc()", "undefined", "undefined");

  auto countMajorGc = [](v8::Isolate*, v8::GCType, v8::GCCallbackFlags, void* data) {
    ++*static_cast<uint*>(data);
  };
  e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    lock.v8Isolate->AddGCEpilogueCallback(countMajorGc, &majorGcCount, v8::kGCTypeMarkSweepCompact);
  });
  KJ_DEFER(e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    lock.v8Isolate->RemoveGCEpilogueCallback(countMajorGc, &majorGcCount);
  }));

  e.expectEval("(() => { for (let i = 0; i < 1000000; ++i) new NumberBox(i); })()", "undefined",
      "undefined");
  KJ_EXPECT(majorGcCount > 0);
}

// Wrappable bytes charged in this isolate, reported to V8 or not.
int64_t wrappableBytes(Evaluator<TraceTestContext, TraceTestIsolate>& e) {
  return e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    return HeapTracer::getTracer(lock.v8Isolate).getChargedWrappableBytes();
  });
}

int64_t reportedWrappableBytes(Evaluator<TraceTestContext, TraceTestIsolate>& e) {
  return e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    return HeapTracer::getTracer(lock.v8Isolate).getReportedWrappableBytes();
  });
}

constexpr int kKeptBoxes = 10000;

// Keeps kKeptBoxes NumberBoxes alive in `globalThis.keep`.
void keepBoxes(Evaluator<TraceTestContext, TraceTestIsolate>& e) {
  e.expectEval(kj::str("globalThis.keep = []; for (let i = 0; i < ", kKeptBoxes,
                   "; ++i) keep.push(new NumberBox(i));"),
      "number", kj::str(kKeptBoxes));
}

KJ_TEST("wrappables are charged as external memory while they live") {
  util::Autogate::initAutogateForTest({util::AutogateKey::WRAPPABLE_EXTERNAL_MEMORY});
  KJ_DEFER(util::Autogate::deinitAutogate());
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  e.expectEval("gc()", "undefined", "undefined");
  int64_t before = wrappableBytes(e);

  // Kept alive, so each is charged and none is released yet.
  keepBoxes(e);
  int64_t live = wrappableBytes(e) - before;
  KJ_EXPECT(live >= int64_t(kKeptBoxes * sizeof(NumberBox)), live);

  // The charge is reported to V8 in batches, so at most one batch is still pending.
  e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    int64_t pending = HeapTracer::getTracer(lock.v8Isolate).getUnreportedWrappableBytesForTest();
    KJ_EXPECT(pending < HeapTracer::kExternalMemoryFlushBytes, pending);
  });

  // Collected, so the charges are released again, with slack for a few boxes the GCs leave alive.
  // The GC is requested from C++ without stack scanning, as a stale stack slot that points at
  // `keep` would keep every box alive. The loop in keepBoxes() may still have an optimizing
  // compile job pending on a background thread, and that job keeps every box alive too, so it is
  // first aborted and waited for.
  e.expectEval("keep = null", "object", "null");
  e.getIsolate().runInLockScope([&](TraceTestIsolate::Lock& lock) {
    lock.v8Isolate->ClearCachesForTesting();
    for (int i = 0; i < 2; ++i) {
      lock.v8Isolate->RequestGarbageCollectionForTesting(
          v8::Isolate::kFullGarbageCollection, v8::StackState::kNoHeapPointers);
    }
  });
  int64_t remaining = wrappableBytes(e) - before;
  KJ_EXPECT(remaining < int64_t(sizeof(NumberBox)) * 16, remaining);
}

KJ_TEST("wrappables are counted but not reported to V8 when the autogate is off") {
  util::Autogate::initAutogateNamesForTest({}, util::IgnoreAllAutogatesEnv::YES);
  KJ_DEFER(util::Autogate::deinitAutogate());
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);
  int64_t before = wrappableBytes(e);
  int64_t reportedBefore = reportedWrappableBytes(e);
  keepBoxes(e);
  int64_t live = wrappableBytes(e) - before;
  KJ_EXPECT(live >= int64_t(kKeptBoxes * sizeof(NumberBox)), live);
  KJ_EXPECT(reportedWrappableBytes(e) == reportedBefore);
}

}  // namespace
}  // namespace workerd::jsg::test
