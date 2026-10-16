// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-context.h"
#include "jsg-test.h"

#include <workerd/tests/bench-tools.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct AsyncContextBenchmarkContext final: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(AsyncContextBenchmarkContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(AsyncContextBenchmarkIsolate, AsyncContextBenchmarkContext);

enum class ScopeLayout { SEPARATE, COMBINED };

template <ScopeLayout layout, typename Callback>
void withTraceScopes(Lock& js,
    const kj::Arc<AsyncContextFrame::StorageKey>& internalKey,
    const kj::Arc<AsyncContextFrame::StorageKey>& userKey,
    Callback&& callback) {
  auto value = v8::Undefined(js.v8Isolate).As<v8::Value>();
  if constexpr (layout == ScopeLayout::SEPARATE) {
    AsyncContextFrame::StorageScope internalScope(js, internalKey.addRef(), js.v8Ref(value));
    AsyncContextFrame::StorageScope userScope(js, userKey.addRef(), js.v8Ref(value));
    callback();
  } else {
    AsyncContextFrame::StorageEntry entries[] = {
      {internalKey.addRef(), js.v8Ref(value)},
      {userKey.addRef(), js.v8Ref(value)},
    };
    AsyncContextFrame::StorageScope scope(js, entries);
    callback();
  }
}

template <ScopeLayout layout>
void traceScopes(benchmark::State& state) {
  Evaluator<AsyncContextBenchmarkContext, AsyncContextBenchmarkIsolate> evaluator(v8System);
  evaluator.run([&](auto& js) {
    auto internalKey = kj::arc<AsyncContextFrame::StorageKey>();
    auto userKey = kj::arc<AsyncContextFrame::StorageKey>();
    kj::Vector<AsyncContextFrame::StorageEntry> inheritedEntries;
    for (auto i: kj::zeroTo(static_cast<int>(state.range(0)))) {
      inheritedEntries.add(AsyncContextFrame::StorageEntry(kj::arc<AsyncContextFrame::StorageKey>(),
          js.v8Ref(v8::Integer::New(js.v8Isolate, i).template As<v8::Value>())));
    }
    kj::Maybe<AsyncContextFrame::StorageScope> inheritedScope;
    if (!inheritedEntries.empty()) {
      inheritedScope.emplace(js, inheritedEntries.asPtr());
    }
    auto run = [&]() {
      v8::HandleScope handleScope(js.v8Isolate);
      withTraceScopes<layout>(js, internalKey, userKey,
          [&]() { benchmark::DoNotOptimize(&KJ_ASSERT_NONNULL(AsyncContextFrame::current(js))); });
    };
    for (auto _: state) {
      run();
    }
    bench::recordAllocationStats(state, run);
  });
  state.SetItemsProcessed(state.iterations());
}

void SeparateTraceScopes(benchmark::State& state) {
  traceScopes<ScopeLayout::SEPARATE>(state);
}

void CombinedTraceScopes(benchmark::State& state) {
  traceScopes<ScopeLayout::COMBINED>(state);
}

WD_BENCHMARK(SeparateTraceScopes)->Arg(0)->Arg(4)->Arg(16);
WD_BENCHMARK(CombinedTraceScopes)->Arg(0)->Arg(4)->Arg(16);

}  // namespace
}  // namespace workerd::jsg::test
