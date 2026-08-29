// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <workerd/tests/bench-tools.h>

namespace workerd::jsg::test {
namespace {

class DispatchTarget final: public Object {
 public:
  uint32_t call() {
    return ++value;
  }

  uint32_t callWith(uint32_t increment) {
    return value += increment;
  }

  JSG_RESOURCE_TYPE(DispatchTarget) {
    JSG_METHOD(call);
    JSG_METHOD(callWith);
  }

 private:
  uint32_t value = 0;
};

class DispatchContext final: public Object, public ContextGlobal {
 public:
  Ref<DispatchTarget> makeTarget(Lock& js) {
    return js.alloc<DispatchTarget>();
  }

  JSG_RESOURCE_TYPE(DispatchContext) {
    JSG_NESTED_TYPE(DispatchTarget);
    JSG_METHOD(makeTarget);
  }
};

JSG_DECLARE_DEBUG_ISOLATE_TYPE(SlowDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(SlowArgDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(FastDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(FastArgDispatchIsolate, DispatchContext, DispatchTarget);

V8System v8System({"--allow-natives-syntax"_kj});

constexpr uint32_t CALLS_PER_BATCH = 10000;

template <typename Isolate, bool WITH_ARGUMENT = false>
void runDispatchBenchmark(benchmark::State& state, bool fastApiEnabled) {
  JsgConfig config = {
    .fastApiEnabled = fastApiEnabled,
  };
  Evaluator<DispatchContext, Isolate, JsgConfig> evaluator(v8System, config);

  evaluator.run([&](auto& js) {
    auto source = WITH_ARGUMENT ? R"(
      const target = makeTarget();
      function dispatchBatch(count) {
        let result;
        for (let i = 0; i < count; ++i) {
          result = target.callWith(1);
        }
        return result;
      }
      %PrepareFunctionForOptimization(dispatchBatch);
      dispatchBatch(10000);
      %OptimizeFunctionOnNextCall(dispatchBatch);
      dispatchBatch(10000);
      dispatchBatch;
    )"_kj
                                : R"(
      const target = makeTarget();
      function dispatchBatch(count) {
        let result;
        for (let i = 0; i < count; ++i) {
          result = target.call();
        }
        return result;
      }
      %PrepareFunctionForOptimization(dispatchBatch);
      dispatchBatch(10000);
      %OptimizeFunctionOnNextCall(dispatchBatch);
      dispatchBatch(10000);
      dispatchBatch;
    )"_kj;
    auto script = check(v8::Script::Compile(js.v8Context(), js.str(source)));
    auto function = check(script->Run(js.v8Context())).template As<v8::Function>();
    auto count = v8::Integer::NewFromUnsigned(js.v8Isolate, CALLS_PER_BATCH);

    callCounter.reset();
    for (auto _: state) {
      v8::Local<v8::Value> args[] = {count};
      auto result = check(function->Call(js.v8Context(), js.v8Context()->Global(), 1, args));
      benchmark::DoNotOptimize(*result);
    }

    state.SetItemsProcessed(state.iterations() * CALLS_PER_BATCH);
    if (fastApiEnabled && callCounter.fast == 0) {
      state.SkipWithError("V8 did not use the Fast API callback");
    }
  });
}

void NativeDispatchSlow(benchmark::State& state) {
  runDispatchBenchmark<SlowDispatchIsolate>(state, false);
}

void NativeDispatchSlowArg(benchmark::State& state) {
  runDispatchBenchmark<SlowArgDispatchIsolate, true>(state, false);
}

void NativeDispatchFast(benchmark::State& state) {
  runDispatchBenchmark<FastDispatchIsolate>(state, true);
}

void NativeDispatchFastArg(benchmark::State& state) {
  runDispatchBenchmark<FastArgDispatchIsolate, true>(state, true);
}

WD_BENCHMARK(NativeDispatchSlow);
WD_BENCHMARK(NativeDispatchSlowArg);
WD_BENCHMARK(NativeDispatchFast);
WD_BENCHMARK(NativeDispatchFastArg);

}  // namespace
}  // namespace workerd::jsg::test
