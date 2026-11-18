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

  // Returning a jsg::Promise from a method makes the promise keep the method's object alive.
  jsg::Promise<kj::String> settle(jsg::Lock& js) {
    return js.resolvedPromise(kj::str("value"));
  }

  JSG_RESOURCE_TYPE(GcDetector) {
    JSG_READONLY_PROTOTYPE_PROPERTY(siblingCollected, getSiblingCollected);
    JSG_METHOD(touch);
    JSG_METHOD(settle);
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

  JSG_RESOURCE_TYPE(InstancePropertyGcDetector) {
    JSG_READONLY_PROTOTYPE_PROPERTY(siblingCollected, getSiblingCollected);
    JSG_READONLY_INSTANCE_PROPERTY(eager, getEager);
    JSG_LAZY_INSTANCE_PROPERTY(lazy, getLazy);
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

  // Sets `strongRef` to a new NumberBox that has no wrapper yet.
  void makeStrongRef(jsg::Lock& js, double value) {
    strongRef = js.alloc<NumberBox>(value);
  }

  // NumberBoxes held strongly, without tracing, and returned to JavaScript as an ArrayPtr, whose
  // wrapper wraps each element without consuming it.
  kj::Array<jsg::Ref<NumberBox>> boxes;

  void makeBoxes(jsg::Lock& js) {
    boxes = kj::arr(js.alloc<NumberBox>(1), js.alloc<NumberBox>(2));
  }

  kj::ArrayPtr<jsg::Ref<NumberBox>> getBoxes() {
    return boxes;
  }

  kj::Array<jsg::Ref<GcDetector>> makeGcDetectorPair(jsg::Lock& js) {
    auto obj1 = js.alloc<GcDetector>();
    auto obj2 = js.alloc<GcDetector>();
    obj1->sibling = *obj2;
    obj2->sibling = *obj1;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
  }

  kj::Array<jsg::Ref<InstancePropertyGcDetector>> makeInstancePropertyGcDetectorPair(
      jsg::Lock& js) {
    auto obj1 = js.alloc<InstancePropertyGcDetector>();
    auto obj2 = js.alloc<InstancePropertyGcDetector>();
    obj1->sibling = *obj2;
    obj2->sibling = *obj1;
    return kj::arr(kj::mv(obj1), kj::mv(obj2));
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

  // The number of wrappers this isolate currently has attached, whatever their type.
  uint liveWrapperCount(jsg::Lock& js) {
    uint count = 0;
    HeapTracer::getTracer(js.v8Isolate)
        .forEachLiveWrapper(
            [&](jsg::Wrappable&, v8::Object::Wrappable&, v8::CppHeapPointerTag) { ++count; });
    return count;
  }

  // Attaches C++ continuations to settled promises and drops the results, so the continuations
  // run at the next microtask checkpoint and are garbage afterwards.
  void attachContinuations(jsg::Lock& js) {
    js.resolvedPromise().then(js, [](jsg::Lock&) {});
    js.resolvedPromise(kj::str("value")).then(js, [](jsg::Lock&, kj::String) {
    }, [](jsg::Lock&, jsg::Value) {});
    js.rejectedPromise<void>(js.v8Undefined()).catch_(js, [](jsg::Lock&, jsg::Value) {});
  }

  // Returning a jsg::Promise to JavaScript attaches a continuation that converts its value.
  jsg::Promise<kj::String> resolvedString(jsg::Lock& js) {
    return js.resolvedPromise(kj::str("value"));
  }

  // Accepting a JavaScript promise as a jsg::Promise attaches a continuation that converts its
  // value.
  void acceptStringPromise(jsg::Lock& js, jsg::Promise<kj::String> promise) {
    promise.then(js, [](jsg::Lock&, kj::String) {});
  }

  void runMicrotasks(jsg::Lock& js) {
    js.runMicrotasks();
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
    JSG_METHOD(makeStrongRef);
    JSG_METHOD(makeBoxes);
    JSG_METHOD(getBoxes);
    JSG_METHOD(makeGcDetectorPair);
    JSG_METHOD(makeGcDetectorBoxPair);
    JSG_METHOD(wrapOpaquely);
    JSG_METHOD(makeInstancePropertyGcDetectorPair);
    JSG_METHOD(liveWrapperCount);
    JSG_METHOD(attachContinuations);
    JSG_METHOD(resolvedString);
    JSG_METHOD(acceptStringPromise);
    JSG_METHOD(runMicrotasks);
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

  // Test that minor GC collects the wrappers of promise continuations that have run, along with
  // the opaque wrappers that carried their values.
  e.expectEval(R"(
    let pair = makeGcDetectorPair();
    let b = pair[1];
    let before = liveWrapperCount();
    attachContinuations();
    resolvedString().then(() => {});
    acceptStringPromise(Promise.resolve("value"));
    pair[0].settle().then(() => {});
    pair = null;
    runMicrotasks();
    // The first pass drops the wrappers and the second whatever they still referenced.
    gc({type: "minor"});
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect the object a promise kept alive");
    // pair[0] was counted in `before`.
    let survivors = liveWrapperCount() - (before - 1);
    assert(survivors == 0, `${survivors} wrappers survived minor GCs`);
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

KJ_TEST("minor GC collects wrappers with instance properties") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // The instance properties alone don't make the wrapper "modified".
  e.expectEval(R"(
    let pair = makeInstancePropertyGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    a = null;
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect a wrapper with instance properties");
  )",
      "undefined", "undefined");

  // Reading an eager instance property leaves the wrapper as it was.
  e.expectEval(R"(
    let pair = makeInstancePropertyGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    (() => {
      assert(a.eager === 1);
    })();
    a = null;
    gc({type: "minor"});
    assert(b.siblingCollected, "minor GC did not collect a wrapper after an eager read");
  )",
      "undefined", "undefined");

  // Reading a lazy instance property stores its value on the wrapper, which modifies it.
  e.expectEval(R"(
    let pair = makeInstancePropertyGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    (() => {
      assert(a.lazy === 2);
    })();
    a = null;
    gc({type: "minor"});
    assert(!b.siblingCollected, "minor GC collected a wrapper holding a lazy property's value");
  )",
      "undefined", "undefined");

  // So does deleting an instance property.
  e.expectEval(R"(
    let pair = makeInstancePropertyGcDetectorPair();
    let a = pair[0];
    let b = pair[1];
    pair = null;
    delete a.lazy;
    a = null;
    gc({type: "minor"});
    assert(!b.siblingCollected, "minor GC collected a wrapper missing an instance property");
  )",
      "undefined", "undefined");
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

KJ_TEST("a wrapper made by consuming one of two strong Refs stays alive") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // Reading `strongRef` wraps a new Ref to an object with no wrapper, consuming that Ref. The
  // context's own Ref is still strong, so the wrapper must survive both kinds of GC with its
  // properties.
  e.expectEval(R"(
    makeStrongRef(123);
    strongRef.note = "kept";
    gc({type: "minor"});
    gc();
    strongRef.note + " " + strongRef.value
  )",
      "string", "kept 123");
}

KJ_TEST("wrapping an ArrayPtr of strong Refs leaves them keeping their wrappers alive") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  // The context still holds each box strongly once JavaScript has dropped the array, so a box's
  // wrapper must survive both kinds of GC with its properties.
  e.expectEval(R"(
    makeBoxes();
    getBoxes()[0].note = "kept";
    gc({type: "minor"});
    gc();
    getBoxes()[0].note + " " + getBoxes()[0].value
  )",
      "string", "kept 1");
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

}  // namespace
}  // namespace workerd::jsg::test
