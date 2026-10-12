// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// CI-only measurement harness; excluded from the code-cache PR.
#include "jsg-test.h"
#include "script.h"

#include <kj/filesystem.h>
#include <kj/time.h>

#include <cstdio>
#include <memory>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct BenchmarkContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(BenchmarkContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(BenchmarkIsolate, BenchmarkContext);

void inIsolate(kj::StringPtr shim, kj::FunctionParam<void(jsg::Lock&)> fn) {
  BenchmarkIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](BenchmarkIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<BenchmarkContext>().getHandle(lock), [&](jsg::Lock& js) {
      NonModuleScript::compile(js, "globalThis.print = () => {};"_kj).run(js);
      NonModuleScript::compile(js, shim).run(js);
      fn(js);
    });
  });
}

enum class Mode { AFTER_COMPILE, AFTER_TOP_LEVEL, EAGER };

double milliseconds(kj::Duration elapsed) {
  return (elapsed / kj::NANOSECONDS) / 1e6;
}

struct Produced {
  kj::Array<kj::byte> bytes;
  double compileMs;
  double serializeMs;
  double loadMs;
};

Produced produce(kj::StringPtr code, kj::StringPtr shim, Mode mode) {
  Produced result{};
  inIsolate(shim, [&](jsg::Lock& js) {
    auto& clock = kj::systemPreciseMonotonicClock();
    v8::ScriptOrigin origin(js.str("benchmark.mjs"_kj), 0, 0, false, -1, {}, false, false, true);
    v8::ScriptCompiler::Source source(js.str(code), origin);
    auto start = clock.now();
    auto module = check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source,
        mode == Mode::EAGER ? v8::ScriptCompiler::kEagerCompile
                            : v8::ScriptCompiler::kNoCompileOptions));
    result.compileMs = milliseconds(clock.now() - start);
    if (mode == Mode::AFTER_TOP_LEVEL) instantiateModule(js, module);
    auto serializeStart = clock.now();
    std::unique_ptr<v8::ScriptCompiler::CachedData> data(
        v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
    result.serializeMs = milliseconds(clock.now() - serializeStart);
    KJ_REQUIRE(data != nullptr && data->length > 0);
    result.bytes = kj::heapArray<kj::byte>(data->data, data->length);
    if (mode != Mode::AFTER_TOP_LEVEL) instantiateModule(js, module);
    result.loadMs = milliseconds(clock.now() - start);
  });
  return result;
}

void consume(kj::StringPtr code, kj::StringPtr shim, const Produced& cache, Mode mode, uint run) {
  inIsolate(shim, [&](jsg::Lock& js) {
    auto& clock = kj::systemPreciseMonotonicClock();
    v8::ScriptOrigin origin(js.str("benchmark.mjs"_kj), 0, 0, false, -1, {}, false, false, true);
    auto data = new v8::ScriptCompiler::CachedData(cache.bytes.begin(),
        static_cast<int>(cache.bytes.size()), v8::ScriptCompiler::CachedData::BufferNotOwned);
    v8::ScriptCompiler::Source source(js.str(code), origin, data);
    auto start = clock.now();
    auto module = check(v8::ScriptCompiler::CompileModule(
        js.v8Isolate, &source, v8::ScriptCompiler::kConsumeCodeCache));
    auto deserializeMs = milliseconds(clock.now() - start);
    KJ_EXPECT(!source.GetCachedData()->rejected);
    instantiateModule(js, module);
    auto loadMs = milliseconds(clock.now() - start);
    const char* name = mode == Mode::AFTER_COMPILE ? "after-compile"
        : mode == Mode::AFTER_TOP_LEVEL            ? "after-top-level"
                                                   : "eager";
    fprintf(stderr,
        "BENCHMARK mode=%s run=%u source_bytes=%zu cache_bytes=%zu "
        "compile_ms=%.3f serialize_ms=%.3f deserialize_ms=%.3f "
        "producer_load_ms=%.3f consumer_load_ms=%.3f\n",
        name, run, code.size(), cache.bytes.size(), cache.compileMs, cache.serializeMs,
        deserializeMs, cache.loadMs, loadMs);
  });
}

KJ_TEST("Measure direct eager compilation and startup code cache production") {
  auto fs = kj::newDiskFilesystem();
  auto code = fs->getCurrent()
                  .openFile(kj::Path::parse("src/workerd/jsg/benchmark/hono.mjs"))
                  ->readAllText();
  auto shim = fs->getCurrent()
                  .openFile(kj::Path::parse("src/workerd/jsg/benchmark/shim.js"))
                  ->readAllText();
  fprintf(stderr, "BENCHMARK v8=%s\n", v8::V8::GetVersion());
  // Warm the process before the five interleaved rounds.
  produce(code, shim, Mode::AFTER_TOP_LEVEL);
  for (uint run = 1; run <= 5; ++run) {
    for (uint i = 0; i < 3; ++i) {
      auto mode = static_cast<Mode>((i + run - 1) % 3);
      auto cache = produce(code, shim, mode);
      consume(code, shim, cache, mode, run);
    }
  }
}

}  // namespace
}  // namespace workerd::jsg::test
