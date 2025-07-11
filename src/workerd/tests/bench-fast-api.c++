// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/bench-tools.h>

namespace workerd {
namespace {

class FastMethodContext: public jsg::Object, public jsg::ContextGlobal {
 public:
  int32_t slowAdd(int32_t a, int32_t b) {
    return a + b;
  }

  int32_t slowAddWithLock(jsg::Lock& js, int32_t a, v8::Local<v8::Value> b) {
    int32_t bValue = b.As<v8::Int32>()->Value();
    return a + bValue;
  }

  JSG_RESOURCE_TYPE(FastMethodContext) {
    JSG_METHOD(slowAdd);
    JSG_METHOD(slowAddWithLock);
  }
};

JSG_DECLARE_ISOLATE_TYPE(FastMethodIsolate, FastMethodContext);

jsg::V8System system;

struct FastMethodFixture: public benchmark::Fixture {
  virtual ~FastMethodFixture() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {}

  void TearDown(benchmark::State& state) noexcept(true) override {}
};

// Benchmark the slow method
BENCHMARK_F(FastMethodFixture, SlowAPI)(benchmark::State& state) {
  FastMethodIsolate isolate(system, kj::heap<jsg::IsolateObserver>(), {});
  auto code =
      "var result = 0; for (let i = 0; i < 100000; i++) { result += slowAdd(2, 3); } result"_kj;

  isolate.runInLockScope([&](FastMethodIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<FastMethodContext>();

    return JSG_WITHIN_CONTEXT_SCOPE(
        isolateLock, context.getHandle(isolateLock), [&](jsg::Lock& js) {
      auto source = jsg::v8Str(js.v8Isolate, code);
      auto script = jsg::check(v8::Script::Compile(js.v8Context(), source));

      for (auto _: state) {
        benchmark::DoNotOptimize(jsg::check(script->Run(js.v8Context())));
      }
    });
  });
}

// Benchmark the slow method with Lock& parameter
BENCHMARK_F(FastMethodFixture, SlowAPIWithLock)(benchmark::State& state) {
  FastMethodIsolate isolate(system, kj::heap<jsg::IsolateObserver>(), {});
  auto code =
      "var result = 0; for (let i = 0; i < 100000; i++) { result += slowAddWithLock(2, 3); } result"_kj;

  isolate.runInLockScope([&](FastMethodIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<FastMethodContext>();

    return JSG_WITHIN_CONTEXT_SCOPE(
        isolateLock, context.getHandle(isolateLock), [&](jsg::Lock& js) {
      auto source = jsg::v8Str(js.v8Isolate, code);
      auto script = jsg::check(v8::Script::Compile(js.v8Context(), source));

      for (auto _: state) {
        benchmark::DoNotOptimize(jsg::check(script->Run(js.v8Context())));
      }
    });
  });
}

}  // namespace
}  // namespace workerd
