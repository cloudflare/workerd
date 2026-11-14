// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/form-data.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

namespace workerd {
namespace {

struct FormDataSerialization: public benchmark::Fixture {
  virtual ~FormDataSerialization() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>();
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
};

BENCHMARK_DEFINE_F(FormDataSerialization, singleFile)(benchmark::State& state) {
  static constexpr kj::StringPtr BOUNDARY = "0123456789abcdef0123456789abcdef"_kj;
  const size_t payloadSize = state.range(0);

  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto bytes = jsg::JsUint8Array::create(js, payloadSize);
    memset(bytes.asArrayPtr().begin(), 'x', payloadSize);

    auto file = js.alloc<api::File>(js, jsg::JsBufferSource(bytes), kj::str("upload.bin"),
        kj::str("application/octet-stream"), 0);
    auto formData = js.alloc<api::FormData>();
    formData->append(js, kj::str("file"), kj::mv(file), kj::none);

    for (auto _: state) {
      auto serialized = formData->serialize(BOUNDARY);
      benchmark::DoNotOptimize(serialized.begin());
      benchmark::ClobberMemory();
    }
  });

  state.SetBytesProcessed(state.iterations() * payloadSize);
}

BENCHMARK_REGISTER_F(FormDataSerialization, singleFile)
    ->Arg(1024)
    ->Arg(1024 * 1024)
    ->Arg(16 * 1024 * 1024)
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace workerd
