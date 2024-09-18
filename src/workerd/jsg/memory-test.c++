// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/memory.h>
#include <workerd/jsg/setup.h>

#include <v8-profiler.h>

#include <kj/map.h>
#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct Foo: public Object {
  kj::String bar = kj::str("test");

  JSG_RESOURCE_TYPE(Foo) {}
  void visitForMemoryInfo(MemoryTracker& tracker) const {
    tracker.trackField("bar", bar);
  }
};

struct MemoryTrackerContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(MemoryTrackerContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(MemoryTrackerIsolate, MemoryTrackerContext, Foo);

void runTest(auto callback) {
  MemoryTrackerIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](MemoryTrackerIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.newContext<MemoryTrackerContext>().getHandle(lock),
        [&](jsg::Lock& js) { callback(js, lock.getTypeHandler<Ref<Foo>>()); });
  });
}

KJ_TEST("MemoryTracker test") {

  // Verifies that workerd details are included in the heapsnapshot.
  // This is not a comprehensive test of the heapsnapshot content,
  // it is designed just to make sure that we are, in fact, publishing
  // internal details to the snapshot.

  runTest([&](jsg::Lock& js, const TypeHandler<Ref<Foo>>& fooHandler) {
    kj::Vector<char> serialized;
    HeapSnapshotActivity activity([](auto, auto) { return true; });
    HeapSnapshotWriter writer([&](kj::Maybe<kj::ArrayPtr<char>> maybeChunk) {
      KJ_IF_SOME(chunk, maybeChunk) {
        serialized.addAll(chunk);
      }
      return true;
    });

    IsolateBase& base = IsolateBase::from(js.v8Isolate);
    base.getUuid();

    auto foo = fooHandler.wrap(js, alloc<Foo>());
    KJ_ASSERT(foo->IsObject());

    auto profiler = js.v8Isolate->GetHeapProfiler();

    HeapSnapshotDeleter deleter;

    auto snapshot = kj::Own<const v8::HeapSnapshot>(
        profiler->TakeHeapSnapshot(&activity, nullptr, true, true), deleter);
    snapshot->Serialize(&writer, v8::HeapSnapshot::kJSON);

    auto parsed = js.parseJson(serialized.asPtr());
    JsValue value = JsValue(parsed.getHandle(js));
    KJ_ASSERT(value.isObject());

    JsObject obj = KJ_ASSERT_NONNULL(value.tryCast<JsObject>());

    auto strings = obj.get(js, "strings");
    KJ_ASSERT(strings.isArray());

    JsArray array = KJ_ASSERT_NONNULL(strings.tryCast<JsArray>());

    size_t count = 0;

    kj::HashSet<kj::String> checks;
    checks.insert(kj::str("workerd / IsolateBase"));
    checks.insert(kj::str("workerd / kj::String"));
    checks.insert(kj::str("workerd / HeapTracer"));
    checks.insert(kj::str("workerd / CppgcShim"));
    checks.insert(kj::str("workerd / MemoryTrackerContext"));
    checks.insert(kj::str("workerd / Foo"));

    // Find what we're looking for... this is slow but, you know
    for (size_t n = 0; n < array.size(); n++) {
      JsValue check = array.get(js, n);
      auto str = check.toString(js);
      if (str.startsWith("workerd /")) {
        count++;
        KJ_ASSERT(checks.find(str) != kj::none);
      }
    }
    KJ_ASSERT(count == checks.size());
  });
}

}  // namespace
}  // namespace workerd::jsg::test
