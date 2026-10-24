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

  static uint32_t staticCall(uint32_t x) {
    return x + 1;
  }

  uint32_t getCount() const {
    return value;
  }

  void setCount(uint32_t count) {
    value = count;
  }

  JSG_RESOURCE_TYPE(DispatchTarget) {
    JSG_METHOD(call);
    JSG_METHOD(callWith);
    JSG_STATIC_METHOD(staticCall);
    JSG_PROTOTYPE_PROPERTY(count, getCount, setCount);
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
JSG_DECLARE_DEBUG_ISOLATE_TYPE(SlowStaticDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(FastStaticDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(SlowGetterDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(FastGetterDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(SlowSetterDispatchIsolate, DispatchContext, DispatchTarget);
JSG_DECLARE_DEBUG_ISOLATE_TYPE(FastSetterDispatchIsolate, DispatchContext, DispatchTarget);

V8System v8System({"--allow-natives-syntax"_kj});

constexpr uint32_t CALLS_PER_BATCH = 10000;

template <typename Isolate>
void runDispatchBenchmark(benchmark::State& state, bool fastApiEnabled, kj::StringPtr call) {
  JsgConfig config = {
    .fastApiEnabled = fastApiEnabled,
  };
  Evaluator<DispatchContext, Isolate, JsgConfig> evaluator(v8System, config);

  evaluator.run([&](auto& js) {
    auto source = kj::str(R"(
      const target = makeTarget();
      const Target = target.constructor;
      function dispatchBatch(count) {
        let result;
        for (let i = 0; i < count; ++i) {
          result = )",
        call, R"(;
        }
        return result;
      }
      %PrepareFunctionForOptimization(dispatchBatch);
      dispatchBatch(10000);
      %OptimizeFunctionOnNextCall(dispatchBatch);
      dispatchBatch(10000);
      dispatchBatch;
    )");
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
  runDispatchBenchmark<SlowDispatchIsolate>(state, false, "target.call()");
}

void NativeDispatchSlowArg(benchmark::State& state) {
  runDispatchBenchmark<SlowArgDispatchIsolate>(state, false, "target.callWith(1)");
}

void NativeDispatchFast(benchmark::State& state) {
  runDispatchBenchmark<FastDispatchIsolate>(state, true, "target.call()");
}

void NativeDispatchFastArg(benchmark::State& state) {
  runDispatchBenchmark<FastArgDispatchIsolate>(state, true, "target.callWith(1)");
}

void NativeDispatchSlowStatic(benchmark::State& state) {
  runDispatchBenchmark<SlowStaticDispatchIsolate>(state, false, "Target.staticCall(i)");
}

void NativeDispatchFastStatic(benchmark::State& state) {
  runDispatchBenchmark<FastStaticDispatchIsolate>(state, true, "Target.staticCall(i)");
}

void NativeDispatchSlowGetter(benchmark::State& state) {
  runDispatchBenchmark<SlowGetterDispatchIsolate>(state, false, "target.count");
}

void NativeDispatchFastGetter(benchmark::State& state) {
  runDispatchBenchmark<FastGetterDispatchIsolate>(state, true, "target.count");
}

void NativeDispatchSlowSetter(benchmark::State& state) {
  runDispatchBenchmark<SlowSetterDispatchIsolate>(state, false, "(target.count = i)");
}

void NativeDispatchFastSetter(benchmark::State& state) {
  runDispatchBenchmark<FastSetterDispatchIsolate>(state, true, "(target.count = i)");
}

WD_BENCHMARK(NativeDispatchSlow);
WD_BENCHMARK(NativeDispatchSlowArg);
WD_BENCHMARK(NativeDispatchFast);
WD_BENCHMARK(NativeDispatchFastArg);
WD_BENCHMARK(NativeDispatchSlowStatic);
WD_BENCHMARK(NativeDispatchFastStatic);
WD_BENCHMARK(NativeDispatchSlowGetter);
WD_BENCHMARK(NativeDispatchFastGetter);
WD_BENCHMARK(NativeDispatchSlowSetter);
WD_BENCHMARK(NativeDispatchFastSetter);

}  // namespace
}  // namespace workerd::jsg::test
