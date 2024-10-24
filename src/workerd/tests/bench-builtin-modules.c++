// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// A benchmark for loading built-in modules.

namespace workerd {

static CompatibilityFlags::Builder createCompatibilityFlags() {
  static capnp::MallocMessageBuilder flagsBuilder;
  static auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setNodeJsCompat(true);
  featureFlags.setNodeJsCompatV2(true);
  return featureFlags;
}

class BuiltinModulesBenchmark: public benchmark::Fixture {
public:
  virtual ~BuiltinModulesBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    static auto flags = createCompatibilityFlags();
    TestFixture::SetupParams params = {.featureFlags = flags.asReader(), .mainModuleSource = R"(
        import assert from "node:assert";
        export default {
          async fetch(request) {
            let buffer;
            for (let i = 0; i < 100; i++) {
              buffer = await import("node:buffer");
            }
            assert.ok(buffer);
            return new Response("OK");
          },
        };
      )"_kj};
    fixture = kj::heap<TestFixture>(kj::mv(params));
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

protected:
  kj::Own<TestFixture> fixture;
};

BENCHMARK_F(BuiltinModulesBenchmark, request)(benchmark::State& state) {
  for (auto _: state) {
    auto result = fixture->runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "TEST"_kj);
    KJ_EXPECT(result.statusCode == 200);
  }
}

}  // namespace workerd
