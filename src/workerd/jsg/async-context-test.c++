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
  auto script = check(v8::Script::Compile(js.v8Context(), js.str("gc(); gc();"_kj)));
  check(script->Run(js.v8Context()));
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
