// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/iterator.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/type-wrapper.h>

#include <v8-profiler.h>

#include <kj/map.h>
#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct Baz {
  kj::String qux = kj::str("test");

  JSG_MEMORY_INFO(Baz) {
    tracker.trackField("qux", qux);
  }
};

struct Foo: public Object {
  kj::String bar = kj::str("test");
  kj::Maybe<jsg::JsRef<jsg::JsObject>> ref;
  jsg::TracedOwn<Baz> traced = kj::heap<Baz>();
  jsg::TracedOwn<Baz> empty = nullptr;

  Foo(jsg::Lock& js): ref(js.obj().addRef(js)) {}

  JSG_RESOURCE_TYPE(Foo) {}
  void visitForMemoryInfo(MemoryTracker& tracker) const {
    tracker.trackField("bar", bar);
    tracker.trackField("ref", ref);
    tracker.trackField("traced", traced);
    tracker.trackField("empty", empty);
  }
};

// An async iterator whose next() never settles, so that the iterator holds a pending promise.
struct PendingIterable: public Object {
  struct State {
    kj::Maybe<Promise<kj::Maybe<int>>::Resolver> resolver;
    void visitForGc(GcVisitor& visitor) {
      visitor.visit(resolver);
    }
  };

  static Promise<kj::Maybe<int>> nextFunc(Lock& js, State& state) {
    auto paf = js.newPromiseAndResolver<kj::Maybe<int>>();
    state.resolver = kj::mv(paf.resolver);
    return kj::mv(paf.promise);
  }
  static Promise<void> returnFunc(Lock& js, State& state, Optional<int>& value) {
    return js.resolvedPromise();
  }

  JSG_ASYNC_ITERATOR_TYPE(PendingIterator, int, State, nextFunc, returnFunc);

  JSG_RESOURCE_TYPE(PendingIterable) {}
};

struct SyncIterable: public Object {
  struct SyncIteratorState {
    kj::String name = kj::str("state");
    JSG_MEMORY_INFO(SyncIteratorState) {
      tracker.trackField("name", name);
    }
  };

  static kj::Maybe<int> nextFunc(Lock& js, SyncIteratorState& state) {
    return kj::none;
  }

  JSG_ITERATOR_TYPE(SyncIterator, int, SyncIteratorState, nextFunc);

  JSG_RESOURCE_TYPE(SyncIterable) {}
};

struct MemoryTrackerContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(MemoryTrackerContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(MemoryTrackerIsolate,
    MemoryTrackerContext,
    Foo,
    PendingIterable,
    PendingIterable::PendingIterator,
    PendingIterable::PendingIterator::Next,
    SyncIterable,
    SyncIterable::SyncIterator,
    SyncIterable::SyncIterator::Next);

void runTest(auto callback) {
  MemoryTrackerIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](MemoryTrackerIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.newContext<MemoryTrackerContext>().getHandle(lock),
        [&](jsg::Lock& js) { callback(js, lock); });
  });
}

KJ_TEST("MemoryTracker test") {
  // Verifies that workerd details are included in the heapsnapshot.
  // This is not a comprehensive test of the heapsnapshot content,
  // it is designed just to make sure that we are, in fact, publishing
  // internal details to the snapshot.

  runTest([&](jsg::Lock& js, MemoryTrackerIsolate::Lock& lock) {
    kj::Vector<char> serialized;
    HeapSnapshotActivity activity([](auto, auto) { return true; });
    HeapSnapshotWriter writer([&](kj::Maybe<kj::ArrayPtr<char>> maybeChunk) {
      KJ_IF_SOME(chunk, maybeChunk) {
        serialized.addAll(chunk);
      }
      return true;
    });

    auto foo = lock.getTypeHandler<Ref<Foo>>().wrap(js, js.alloc<Foo>(js));
    KJ_ASSERT(foo->IsObject());

    auto iterator = js.alloc<PendingIterable::PendingIterator>(PendingIterable::State{});
    auto pending = iterator->next(js);
    auto iteratorHandle =
        lock.getTypeHandler<Ref<PendingIterable::PendingIterator>>().wrap(js, kj::mv(iterator));
    KJ_ASSERT(iteratorHandle->IsObject());

    auto syncIteratorHandle = lock.getTypeHandler<Ref<SyncIterable::SyncIterator>>().wrap(
        js, js.alloc<SyncIterable::SyncIterator>(SyncIterable::SyncIteratorState{}));
    KJ_ASSERT(syncIteratorHandle->IsObject());

    CompilationObserver observer;
    auto registry = ModuleRegistryImpl<MemoryTrackerIsolate_TypeWrapper>::from(js);
    auto mainPath = kj::Path::parse("main");
    registry->add(mainPath,
        ModuleRegistry::ModuleInfo(js, "main", "export default 1;"_kj.asArray(), observer));
    auto textPath = kj::Path::parse("text");
    registry->add(textPath,
        ModuleRegistry::ModuleInfo(js, "text", kj::none,
            ModuleRegistry::TextModuleInfo(js, v8Str(js.v8Isolate, "hello"_kj))));

    Function<void()> fn([captured = kj::str("captured")](Lock&) {});
    auto fnHandle = lock.getTypeHandler<Function<void()>>().wrap(js, kj::mv(fn));
    KJ_ASSERT(fnHandle->IsFunction());

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

    kj::HashSet<kj::String> checks;
    checks.insert(kj::str("workerd / IsolateBase"));
    checks.insert(kj::str("workerd / kj::String"));
    checks.insert(kj::str("workerd / HeapTracer"));
    checks.insert(kj::str("workerd / CppgcShim"));
    checks.insert(kj::str("workerd / MemoryTrackerContext"));
    checks.insert(kj::str("workerd / Foo"));
    checks.insert(kj::str("ref"));
    checks.insert(kj::str("bar"));
    checks.insert(kj::str("workerd / Baz"));
    checks.insert(kj::str("traced"));
    checks.insert(kj::str("qux"));
    checks.insert(kj::str("workerd / WrappableFunction"));
    checks.insert(kj::str("workerd / func"));
    checks.insert(kj::str("workerd / PendingIterator"));
    checks.insert(kj::str("workerd / AsyncIteratorImpl"));
    checks.insert(kj::str("workerd / pendingStack"));
    checks.insert(kj::str("workerd / Promise"));
    checks.insert(kj::str("workerd / SyncIteratorState"));
    checks.insert(kj::str("moduleRegistry"));
    checks.insert(kj::str("workerd / ModuleRegistry"));
    checks.insert(kj::str("workerd / Entry"));
    checks.insert(kj::str("workerd / ModuleInfo"));
    checks.insert(kj::str("workerd / ValueModuleInfo"));
    checks.insert(kj::str("specifier"));

    // Find what we're looking for... this is slow but, you know
    for (size_t n = 0; n < array.size(); n++) {
      JsValue check = array.get(js, n);
      auto str = check.toString(js);
      KJ_IF_SOME(found, checks.find(str)) {
        checks.erase(found);
      }
    }
    KJ_ASSERT(checks.size() == 0, "missing from the heap snapshot",
        KJ_MAP(c, checks) { return kj::str(c); });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
