// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bench-tools.h"
#include "test-fixture.h"

namespace workerd {
namespace {

enum class ScopeState { LEGACY, CURRENT };

// The legacy baseline uses a combined frame with the original unobserved tracing payloads.
jsg::AsyncContextFrame::StorageScope makeLegacyTraceScope(const TestFixture::Environment& env) {
  static SpanParent unobservedSpan(nullptr);
  auto span = kj::Own<SpanParent>(&unobservedSpan, kj::NullDisposer::instance);
  auto internalValue = jsg::wrapOpaque(env.js.v8Context(), env.context.addObject(kj::mv(span)));
  auto userContext = kj::heap<UserTraceAsyncContext>(
      SpanParent(nullptr), kj::none, kj::none, tracing::TraceFlags(0));
  auto userValue = jsg::wrapOpaque(env.js.v8Context(), env.context.addObject(kj::mv(userContext)));
  jsg::AsyncContextFrame::StorageEntry entries[] = {
    {env.lock.getTraceAsyncContextKey(), env.js.v8Ref(internalValue)},
    {env.lock.getUserTraceAsyncContextKey(), env.js.v8Ref(userValue)},
  };
  return jsg::AsyncContextFrame::StorageScope(env.js, entries);
}

template <ScopeState scopeState>
void unobservedTraceScopes(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Vector<jsg::AsyncContextFrame::StorageEntry> inheritedEntries;
    for (auto i: kj::zeroTo(static_cast<int>(state.range(0)))) {
      inheritedEntries.add(
          jsg::AsyncContextFrame::StorageEntry(kj::arc<jsg::AsyncContextFrame::StorageKey>(),
              env.js.v8Ref(v8::Integer::New(env.isolate, i).As<v8::Value>())));
    }
    kj::Maybe<jsg::AsyncContextFrame::StorageScope> inheritedScope;
    if (!inheritedEntries.empty()) inheritedScope.emplace(env.js, inheritedEntries.asPtr());
    auto run = [&]() {
      v8::HandleScope handleScope(env.isolate);
      auto scope = [&]() {
        if constexpr (scopeState == ScopeState::LEGACY) {
          return makeLegacyTraceScope(env);
        } else {
          return env.context.makeRequestTraceScope(env.lock);
        }
      }();
      benchmark::DoNotOptimize(&KJ_ASSERT_NONNULL(jsg::AsyncContextFrame::current(env.js)));
    };
    for (auto _: state) {
      run();
    }
    bench::recordAllocationStats(state, run);
  });
  state.SetItemsProcessed(state.iterations());
}

void LegacyUnobservedTraceScopes(benchmark::State& state) {
  unobservedTraceScopes<ScopeState::LEGACY>(state);
}

void UnobservedTraceScopes(benchmark::State& state) {
  unobservedTraceScopes<ScopeState::CURRENT>(state);
}

WD_BENCHMARK(LegacyUnobservedTraceScopes)->Arg(0)->Arg(4)->Arg(16);
WD_BENCHMARK(UnobservedTraceScopes)->Arg(0)->Arg(4)->Arg(16);

}  // namespace
}  // namespace workerd
