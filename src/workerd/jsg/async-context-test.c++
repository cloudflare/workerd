// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-context.h"
#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});

struct AsyncContextTestContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(AsyncContextTestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(AsyncContextTestIsolate, AsyncContextTestContext);

void collectGarbage(Lock& js) {
  // Ignore stale native stack pointers after the test releases its owning references.
  for (uint i = 0; i < 2; ++i) {
    js.v8Isolate->RequestGarbageCollectionForTesting(
        v8::Isolate::kFullGarbageCollection, v8::StackState::kNoHeapPointers);
  }
}

AsyncContextFrame::StorageEntry numberEntry(
    Lock& js, const kj::Arc<AsyncContextFrame::StorageKey>& key, int value) {
  return {key.addRef(), js.v8Ref(v8::Integer::New(js.v8Isolate, value).As<v8::Value>())};
}

int storedNumber(Lock& js, AsyncContextFrame& frame, const AsyncContextFrame::StorageKey& key) {
  return KJ_ASSERT_NONNULL(frame.get(key)).getHandle(js).As<v8::Int32>()->Value();
}

KJ_TEST("multi-entry async scopes inherit storage and restore their prior frame") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  evaluator.run([&](auto& js) {
    auto inheritedKey = kj::arc<AsyncContextFrame::StorageKey>();
    auto internalKey = kj::arc<AsyncContextFrame::StorageKey>();
    auto userKey = kj::arc<AsyncContextFrame::StorageKey>();
    AsyncContextFrame::StorageEntry priorEntries[] = {
      numberEntry(js, inheritedKey, 1),
      numberEntry(js, internalKey, 2),
    };
    AsyncContextFrame::StorageScope priorScope(js, priorEntries);
    auto& prior = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
    kj::Maybe<Ref<AsyncContextFrame>> captured;
    {
      AsyncContextFrame::StorageEntry entries[] = {
        numberEntry(js, internalKey, 3),
        numberEntry(js, userKey, 4),
      };
      AsyncContextFrame::StorageScope scope(js, entries);
      auto& current = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
      KJ_EXPECT(storedNumber(js, current, *inheritedKey) == 1);
      KJ_EXPECT(storedNumber(js, current, *internalKey) == 3);
      KJ_EXPECT(storedNumber(js, current, *userKey) == 4);
      KJ_EXPECT(storedNumber(js, prior, *internalKey) == 2);
      KJ_EXPECT(prior.get(*userKey) == kj::none);
      captured = current.addRef();
    }
    KJ_EXPECT(&KJ_ASSERT_NONNULL(AsyncContextFrame::current(js)) == &prior);

    collectGarbage(js);
    {
      AsyncContextFrame::Scope scope(js, captured);
      auto& current = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
      KJ_EXPECT(storedNumber(js, current, *inheritedKey) == 1);
      KJ_EXPECT(storedNumber(js, current, *internalKey) == 3);
      KJ_EXPECT(storedNumber(js, current, *userKey) == 4);
    }
    KJ_EXPECT(&KJ_ASSERT_NONNULL(AsyncContextFrame::current(js)) == &prior);
  });
}

KJ_TEST("multi-entry async scopes ignore dead keys and keep the last value for duplicate keys") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  evaluator.run([&](auto& js) {
    auto deadKey = kj::arc<AsyncContextFrame::StorageKey>();
    auto liveKey = kj::arc<AsyncContextFrame::StorageKey>();
    deadKey->reset();
    v8::Global<v8::Object> deadValue;
    kj::Maybe<AsyncContextFrame::StorageScope> scope;
    {
      v8::HandleScope handleScope(js.v8Isolate);
      auto value = v8::Object::New(js.v8Isolate);
      deadValue.Reset(js.v8Isolate, value);
      deadValue.SetWeak();
      AsyncContextFrame::StorageEntry entries[] = {
        numberEntry(js, liveKey, 1),
        {deadKey.addRef(), js.v8Ref(value.template As<v8::Value>())},
        numberEntry(js, liveKey, 3),
      };
      scope.emplace(js, entries);
    }
    collectGarbage(js);
    KJ_EXPECT(deadValue.IsEmpty());
    auto& current = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
    KJ_EXPECT(storedNumber(js, current, *liveKey) == 3);
    scope = kj::none;
    KJ_EXPECT(AsyncContextFrame::current(js) == kj::none);
  });
}

KJ_TEST("async frame growth preserves inherited values and replaces duplicate keys") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  for (int count: {0, 1, 2, 3, 4, 8, 64}) {
    evaluator.run([&](auto& js) {
      kj::Vector<kj::Arc<AsyncContextFrame::StorageKey>> keys;
      kj::Vector<AsyncContextFrame::StorageEntry> entries;
      for (auto i: kj::zeroTo(count)) {
        keys.add(kj::arc<AsyncContextFrame::StorageKey>());
        entries.add(numberEntry(js, keys.back(), i));
      }
      AsyncContextFrame::StorageScope parentScope(js, entries.asPtr());
      auto& parent = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
      auto newKey = kj::arc<AsyncContextFrame::StorageKey>();
      KJ_EXPECT(parent.get(*newKey) == kj::none);
      for (auto i: kj::zeroTo(count)) {
        KJ_EXPECT(storedNumber(js, parent, *keys[i]) == i);
      }

      kj::Vector<AsyncContextFrame::StorageEntry> updates;
      if (count > 0) {
        updates.add(numberEntry(js, keys.front(), -1));
        updates.add(numberEntry(js, keys.back(), -2));
      }
      updates.add(numberEntry(js, newKey, -3));
      {
        AsyncContextFrame::StorageScope childScope(js, updates.asPtr());
        auto& child = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
        for (auto i: kj::zeroTo(count)) {
          int expected = i;
          if (i == 0) expected = -1;
          if (i == count - 1) expected = -2;
          KJ_EXPECT(storedNumber(js, child, *keys[i]) == expected);
          KJ_EXPECT(storedNumber(js, parent, *keys[i]) == i);
        }
        KJ_EXPECT(storedNumber(js, child, *newKey) == -3);
        KJ_EXPECT(parent.get(*newKey) == kj::none);
      }
      KJ_EXPECT(&KJ_ASSERT_NONNULL(AsyncContextFrame::current(js)) == &parent);
    });
  }
}

KJ_TEST("async frame lookup and inheritance release values belonging to dead keys") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  for (int count: {2, 3, 8}) {
    for (bool cleanOnLookup: {false, true}) {
      evaluator.run([&](auto& js) {
        kj::Vector<kj::Arc<AsyncContextFrame::StorageKey>> keys;
        v8::Global<v8::Object> deadValue;
        kj::Maybe<AsyncContextFrame::StorageScope> parentScope;
        {
          v8::HandleScope handleScope(js.v8Isolate);
          kj::Vector<AsyncContextFrame::StorageEntry> entries;
          for (auto i: kj::zeroTo(count)) {
            keys.add(kj::arc<AsyncContextFrame::StorageKey>());
            if (i == count - 1) {
              auto value = v8::Object::New(js.v8Isolate);
              deadValue.Reset(js.v8Isolate, value);
              deadValue.SetWeak();
              entries.add(AsyncContextFrame::StorageEntry(
                  keys.back().addRef(), js.v8Ref(value.template As<v8::Value>())));
            } else {
              entries.add(numberEntry(js, keys.back(), i));
            }
          }
          parentScope.emplace(js, entries.asPtr());
        }
        for (auto i: kj::range(1, count)) {
          keys[i]->reset();
        }
        auto& parent = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
        if (cleanOnLookup) {
          KJ_EXPECT(storedNumber(js, parent, *keys.front()) == 0);
          collectGarbage(js);
          KJ_EXPECT(deadValue.IsEmpty());
        }
        auto newKey = kj::arc<AsyncContextFrame::StorageKey>();
        {
          AsyncContextFrame::StorageScope childScope(js, newKey.addRef(),
              js.v8Ref(v8::Integer::New(js.v8Isolate, -1).template As<v8::Value>()));
          collectGarbage(js);
          KJ_EXPECT(deadValue.IsEmpty());
          auto& child = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
          KJ_EXPECT(storedNumber(js, child, *keys.front()) == 0);
          KJ_EXPECT(storedNumber(js, child, *newKey) == -1);
        }
        KJ_EXPECT(storedNumber(js, parent, *keys.front()) == 0);
        KJ_EXPECT(parent.get(*newKey) == kj::none);
      });
    }
  }
}

KJ_TEST("captured async frames retain storage values until the frame is released") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  for (int count: {1, 2, 3, 8}) {
    evaluator.run([&](auto& js) {
      auto key = kj::arc<AsyncContextFrame::StorageKey>();
      v8::Global<v8::Object> weakValue;
      kj::Maybe<Ref<AsyncContextFrame>> captured;
      {
        v8::HandleScope handleScope(js.v8Isolate);
        kj::Vector<AsyncContextFrame::StorageEntry> entries;
        auto value = v8::Object::New(js.v8Isolate);
        weakValue.Reset(js.v8Isolate, value);
        weakValue.SetWeak();
        entries.add(AsyncContextFrame::StorageEntry(
            key.addRef(), js.v8Ref(value.template As<v8::Value>())));
        for (int i = 1; i < count; ++i) {
          entries.add(numberEntry(js, kj::arc<AsyncContextFrame::StorageKey>(), i));
        }
        AsyncContextFrame::StorageScope scope(js, entries.asPtr());
        captured = AsyncContextFrame::currentRef(js);
      }
      collectGarbage(js);
      KJ_EXPECT(!weakValue.IsEmpty());
      {
        v8::HandleScope handleScope(js.v8Isolate);
        AsyncContextFrame::Scope scope(js, captured);
        auto& frame = KJ_ASSERT_NONNULL(AsyncContextFrame::current(js));
        auto value = KJ_ASSERT_NONNULL(frame.get(*key)).getHandle(js);
        KJ_EXPECT(value == weakValue.Get(js.v8Isolate));
      }
      captured = kj::none;
      collectGarbage(js);
      KJ_EXPECT(weakValue.IsEmpty());
    });
  }
}

KJ_TEST("AsyncContextFrame::Scope retains its prior frame") {
  Evaluator<AsyncContextTestContext, AsyncContextTestIsolate> evaluator(v8System);
  evaluator.run([&](auto& js) {
    auto key = kj::arc<AsyncContextFrame::StorageKey>();
    kj::Maybe<Ref<AsyncContextFrame>> prior = AsyncContextFrame::create(js,
        AsyncContextFrame::StorageEntry(
            kj::mv(key), js.v8Ref(v8Str(js.v8Isolate, "prior"_kj).template As<v8::Value>())));
    auto weakPrior = KJ_ASSERT_NONNULL(prior).getWeakRef(js);

    auto priorScope = [&]() {
      v8::HandleScope handleScope(js.v8Isolate);
      return kj::heap<AsyncContextFrame::Scope>(js, *KJ_ASSERT_NONNULL(prior).get());
    }();
    {
      auto rootScope = [&]() {
        v8::HandleScope handleScope(js.v8Isolate);
        return kj::heap<AsyncContextFrame::Scope>(js, kj::none);
      }();
      prior = kj::none;
      collectGarbage(js);
      KJ_EXPECT(weakPrior.isAlive(), "scope did not retain the prior async context frame");
    }

    KJ_IF_SOME(current, AsyncContextFrame::current(js)) {
      KJ_IF_SOME(restored, weakPrior.tryGet()) {
        KJ_EXPECT(&current == &restored);
      } else {
        KJ_FAIL_EXPECT("restored async context frame was destroyed");
      }
    } else {
      KJ_FAIL_EXPECT("prior async context frame was not restored");
    }
  });
}

}  // namespace
}  // namespace workerd::jsg::test
