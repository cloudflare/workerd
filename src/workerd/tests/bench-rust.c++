// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/rust/dns/lib.rs.h>
#include <workerd/tests/bench-tools.h>

#include <kj-rs/kj-rs.h>

namespace workerd {
namespace {

class RustContext: public jsg::Object, public jsg::ContextGlobal {
 public:
  kj::String cpp(kj::String value) {
    auto result = workerd::rust::dns::parse_caa_record(value.as<kj_rs::RustUncheckedUtf8>());
    return kj::str(result.value);
  }

  ::rust::String rust(::rust::String value) {
    auto result = workerd::rust::dns::parse_caa_record(value);
    return kj::mv(result.value);
  }

  JSG_RESOURCE_TYPE(RustContext) {
    JSG_METHOD(cpp);
    JSG_METHOD(rust);
  }
};

JSG_DECLARE_ISOLATE_TYPE(FastMethodIsolate, RustContext);

jsg::V8System system;

// Benchmark the slow method
static void KjStringToRustString(benchmark::State& state) {
  FastMethodIsolate isolate(system, kj::heap<jsg::IsolateObserver>(), {});
  auto code =
      "var result = 0; for (let i = 0; i < 500000; i++) { result += cpp('\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67').length; } result"_kj;

  isolate.runInLockScope([&](FastMethodIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<RustContext>();

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
static void v8StringToRustString(benchmark::State& state) {
  FastMethodIsolate isolate(system, kj::heap<jsg::IsolateObserver>(), {});
  auto code =
      "var result = 0; for (let i = 0; i < 500000; i++) { result += rust('\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67').length; } result"_kj;

  isolate.runInLockScope([&](FastMethodIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<RustContext>();

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

BENCHMARK(KjStringToRustString);
BENCHMARK(v8StringToRustString);

}  // namespace
}  // namespace workerd
