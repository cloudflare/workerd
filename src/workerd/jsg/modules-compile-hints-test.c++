// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct HintsContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(HintsContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(HintsIsolate, HintsContext);

// Valid both as a classic script and as a module, so that hints produced by compiling it as a
// script apply to the module: source positions depend only on the text.
constexpr kj::StringPtr SOURCE = "function used() { return 42; }\n"
                                 "function unused() { return 0; }\n"
                                 "globalThis.result = used();\n"_kj;

// V8 reports a function's start as the opening parenthesis of its parameter list.
int usedPosition() {
  return static_cast<int>(KJ_ASSERT_NONNULL(SOURCE.findFirst('(')));
}

v8::MaybeLocal<v8::Module> resolveNothing(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> importAttributes,
    v8::Local<v8::Module> referrer) {
  // The test module has no imports.
  return {};
}

class HintsObserver final: public CompilationObserver {
 public:
  void onCompileHintsConsumed(
      v8::Isolate* isolate, size_t hintCount, size_t eagerFunctionCount) const override {
    ++calls;
    lastHintCount = hintCount;
    lastEagerFunctionCount = eagerFunctionCount;
  }

  mutable uint calls = 0;
  mutable size_t lastHintCount = 0;
  mutable size_t lastEagerFunctionCount = 0;
};

template <typename Func>
void withContext(HintsIsolate& isolate, Func&& func) {
  isolate.runInLockScope([&](HintsIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.newContext<HintsContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) { func(js); });
  });
}

// Compiles SOURCE as a classic script with kProduceCompileHints, runs it, and returns the
// positions of the functions V8 compiled lazily while it ran.
kj::Array<int> produceHints() {
  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  kj::Array<int> result;
  withContext(isolate, [&](jsg::Lock& js) {
    v8::ScriptOrigin origin(v8StrIntern(js.v8Isolate, "producer.js"_kj));
    v8::ScriptCompiler::Source source(v8Str(js.v8Isolate, SOURCE), origin);
    auto script = check(v8::ScriptCompiler::Compile(
        js.v8Context(), &source, v8::ScriptCompiler::kProduceCompileHints));
    check(script->Run(js.v8Context()));
    auto positions = script->GetCompileHintsCollector()->GetCompileHints(js.v8Isolate);
    result = kj::heapArray<int>(positions.data(), positions.size());
  });
  return result;
}

kj::Array<kj::byte> produceCodeCache() {
  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  kj::Array<kj::byte> result;
  withContext(isolate, [&](jsg::Lock& js) {
    CompilationObserver observer;
    ModuleRegistry::ModuleInfo info(js, "main.js", SOURCE.asArray(), nullptr, observer);
    std::unique_ptr<v8::ScriptCompiler::CachedData> cache(
        v8::ScriptCompiler::CreateCodeCache(info.module.getHandle(js)->GetUnboundModuleScript()));
    KJ_ASSERT(cache != nullptr);
    result = kj::heapArray<kj::byte>(cache->data, cache->length);
  });
  return result;
}

// Compiles SOURCE as a module in a fresh isolate, evaluates it, and checks that it ran.
void compileAndEvaluateModule(kj::ArrayPtr<const kj::byte> compileCache,
    kj::ArrayPtr<const int> compileHints,
    const CompilationObserver& observer) {
  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  withContext(isolate, [&](jsg::Lock& js) {
    ModuleRegistry::ModuleInfo info(
        js, "main.js", SOURCE.asArray(), compileCache, observer, compileHints);
    auto module = info.module.getHandle(js);
    check(module->InstantiateModule(js.v8Context(), &resolveNothing));
    check(module->Evaluate(js.v8Context()));
    auto result = check(
        js.v8Context()->Global()->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "result"_kj)));
    KJ_EXPECT(check(result->Int32Value(js.v8Context())) == 42);
  });
}

KJ_TEST("hints produced by running code make V8 compile those functions with the module") {
  auto hints = produceHints();
  // Only `used` ran; `unused` was never compiled.
  KJ_ASSERT(hints.size() == 1, hints.size());
  KJ_EXPECT(hints[0] == usedPosition(), hints[0], usedPosition());

  HintsObserver observer;
  compileAndEvaluateModule(nullptr, hints, observer);
  KJ_EXPECT(observer.calls == 1);
  KJ_EXPECT(observer.lastHintCount == 1);
  // V8 asked about `used` and `unused`, and compiled only `used` eagerly.
  KJ_EXPECT(observer.lastEagerFunctionCount == 1, observer.lastEagerFunctionCount);
}

KJ_TEST("unsorted hints and positions that start no function are tolerated") {
  const int hints[] = {9999, usedPosition(), 3};

  HintsObserver observer;
  compileAndEvaluateModule(nullptr, hints, observer);
  KJ_EXPECT(observer.calls == 1);
  KJ_EXPECT(observer.lastHintCount == 3);
  KJ_EXPECT(observer.lastEagerFunctionCount == 1, observer.lastEagerFunctionCount);
}

KJ_TEST("a code cache takes precedence over compile hints") {
  // V8 rejects combining the two options; CompileModule() would abort.
  KJ_EXPECT(
      !v8::ScriptCompiler::CompileOptionsIsValid(static_cast<v8::ScriptCompiler::CompileOptions>(
          v8::ScriptCompiler::kConsumeCodeCache | v8::ScriptCompiler::kConsumeCompileHints)));

  auto cache = produceCodeCache();
  const int hints[] = {usedPosition()};

  HintsObserver observer;
  compileAndEvaluateModule(cache, hints, observer);
  KJ_EXPECT(observer.calls == 0);
}

KJ_TEST("modules compile without hints") {
  HintsObserver observer;
  compileAndEvaluateModule(nullptr, nullptr, observer);
  KJ_EXPECT(observer.calls == 0);
}

}  // namespace
}  // namespace workerd::jsg::test
