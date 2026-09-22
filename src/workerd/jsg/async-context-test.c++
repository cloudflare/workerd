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
