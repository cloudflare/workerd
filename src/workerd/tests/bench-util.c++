// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

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

static void Util_RecursivelyFreeze(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto obj = jsg::check(v8::JSON::Parse(js.v8Context(), jsg::v8Str(js.v8Isolate, cfProperty)));

    for (auto _: state) {
      for (size_t i = 0; i < 100000; ++i) {
        jsg::recursivelyFreeze(js.v8Context(), obj);
        benchmark::DoNotOptimize(i);
      }
    }
  });
}

WD_BENCHMARK(Util_RecursivelyFreeze);

}  // namespace
}  // namespace workerd
