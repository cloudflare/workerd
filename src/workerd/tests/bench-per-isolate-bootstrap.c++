// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

// Compares the C++ and Rust loaders of the per-isolate bootstrap (the RUST_PER_ISOLATE_BOOTSTRAP
// autogate). Each benchmark takes the loader as its argument: 0 for C++, 1 for Rust.

namespace workerd {
namespace {

TestFixture::SetupParams paramsFor(const benchmark::State& state) {
  TestFixture::SetupParams params;
  if (state.range(0) == 1) {
    params.autogateKeys = kj::arr(util::AutogateKey::RUST_PER_ISOLATE_BOOTSTRAP);
  }
  // Only the gate chosen above, even under WORKERD_ALL_AUTOGATES.
  params.ignoreAllAutogatesEnv = true;
  return params;
}

// A worker from scratch: the isolate, its context and the bootstrap, which runs every script
// `main` requires, and the worker's (trivial) script. The difference between the loaders is the
// bootstrap's.
void newWorker(benchmark::State& state) {
  for (auto _: state) {
    TestFixture fixture(paramsFor(state));
    benchmark::DoNotOptimize(&fixture);
  }
}
BENCHMARK(newWorker)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

// The lookups C++ makes when it calls into the bootstrap's TypeScript: the exports of a loaded
// module, and a function of them, which the loaders cache.
void lookups(benchmark::State& state, auto&& lookup) {
  TestFixture fixture(paramsFor(state));
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    // Loaded on first use; every iteration finds it loaded.
    std::ignore = requireBootstrapExport(js, "url/cpp_exports"_kj);
    KJ_ASSERT(tryGetBootstrapFunction(js, "url/cpp_exports"_kj, "urlHref"_kj) != kj::none);
    for (auto _: state) {
      js.withinHandleScope([&] {
        for (size_t i = 0; i < 1000; ++i) {
          lookup(js);
        }
      });
    }
  });
}

void requireLoadedExport(benchmark::State& state) {
  lookups(state, [](jsg::Lock& js) {
    benchmark::DoNotOptimize(requireBootstrapExport(js, "url/cpp_exports"_kj));
  });
}
BENCHMARK(requireLoadedExport)->Arg(0)->Arg(1);

void getExport(benchmark::State& state) {
  lookups(state, [](jsg::Lock& js) {
    benchmark::DoNotOptimize(tryGetBootstrapExport(js, "url/cpp_exports"_kj));
  });
}
BENCHMARK(getExport)->Arg(0)->Arg(1);

void getFunction(benchmark::State& state) {
  lookups(state, [](jsg::Lock& js) {
    benchmark::DoNotOptimize(tryGetBootstrapFunction(js, "url/cpp_exports"_kj, "urlHref"_kj));
  });
}
BENCHMARK(getFunction)->Arg(0)->Arg(1);

}  // namespace
}  // namespace workerd
