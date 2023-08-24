// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// A benchmark for GlobalScope functionality.

namespace workerd {
namespace {

struct GlobalScopeBenchmark: public benchmark::Fixture {
  virtual ~GlobalScopeBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    TestFixture::SetupParams params = {
      .mainModuleSource = R"(
        export default {
          async fetch(request) {
            return new Response("OK");
          },
        };
      )"_kj};
    fixture = kj::heap<TestFixture>(params);
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
};

BENCHMARK_F(GlobalScopeBenchmark, request)(benchmark::State& state) {
  for (auto _ : state) {
    auto result = fixture->runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "TEST"_kj);
    KJ_EXPECT(result.statusCode == 200);
  }
}

} // namespace
} // namespace workerd
