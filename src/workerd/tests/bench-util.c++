// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>
#include <workerd/tests/bench-tools.h>

namespace workerd {
namespace {

// Ref: https://developers.cloudflare.com/workers/runtime-apis/request/#the-cf-property-requestinitcfproperties
static constexpr kj::StringPtr cfProperty = R"DATA({
  "apps": false,
  "cacheEverything": false,
  "cacheKey": "my-cache-key",
  "cacheTags": ["production", "development"],
  "cacheTtl": 3600,
  "cacheTtlByStatus": { "200-299": 86400, "404": 1, "500-599": 0 },
  "image": null,
  "mirage": true,
  "polish": "lossless",
  "scrapeShield": true,
  "webp": false
})DATA"_kjc;

class UtilContext: public jsg::Object, public jsg::ContextGlobal {
 public:
  uint freezeThis(jsg::Lock& js, v8::Local<v8::Object> value) {
    jsg::recursivelyFreeze(js.v8Context(), value);
    auto names = jsg::check(value->GetPropertyNames(js.v8Context(), v8::KeyCollectionMode::kOwnOnly,
        v8::ALL_PROPERTIES, v8::IndexFilter::kIncludeIndices));
    return names->Length();
  }

  JSG_RESOURCE_TYPE(UtilContext) {
    JSG_METHOD(freezeThis);
  }
};

JSG_DECLARE_ISOLATE_TYPE(UtilIsolate, UtilContext);

jsg::V8System system;

struct UtilFixture: public benchmark::Fixture {
  virtual ~UtilFixture() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {}

  void TearDown(benchmark::State& state) noexcept(true) override {}
};

BENCHMARK_F(UtilFixture, recursivelyFreeze)(benchmark::State& state) {
  UtilIsolate isolate(system, kj::heap<jsg::IsolateObserver>(), {});
  auto code = kj::str("var cfObj = ", cfProperty, "; ",
      "var result = 0; for (let i = 0; i < 10000; i++) { result += freezeThis(cfObj); } result"_kj);

  isolate.runInLockScope([&](UtilIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<UtilContext>();

    return JSG_WITHIN_CONTEXT_SCOPE(
        isolateLock, context.getHandle(isolateLock), [&](jsg::Lock& js) {
      v8::Local<v8::String> source = jsg::v8Str(js.v8Isolate, code);

      // Compile the source code.
      v8::Local<v8::Script> script;
      KJ_ASSERT(v8::Script::Compile(js.v8Context(), source).ToLocal(&script));

      // Run the script to get the result.
      for (auto _: state) {
        benchmark::DoNotOptimize(jsg::check(script->Run(js.v8Context())));
      }
    });
  });
}

}  // namespace
}  // namespace workerd
