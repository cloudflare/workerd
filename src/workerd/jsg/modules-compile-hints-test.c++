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

constexpr kj::StringPtr SOURCE = "function used() { return 42; }\n"
                                 "function unused() { return 0; }\n"
                                 "export function handler() { return used() + 1; }\n"
                                 "export const result = used();\n"_kj;

// Two modules, one importing the other, for the module registry tests.
constexpr kj::StringPtr LIB_SOURCE = "export function square(x) { return x * x; }\n"
                                     "export function cube(x) { return x * x * x; }\n"_kj;
constexpr kj::StringPtr MAIN_SOURCE = "import { square } from './lib.js';\n"
                                      "function unused() { return 0; }\n"
                                      "export function handler() { return square(3) + 1; }\n"
                                      "export const result = square(6);\n"_kj;

// V8 reports a function's start as the opening parenthesis of its parameter list.
int startPosition(kj::StringPtr source, kj::StringPtr functionName) {
  auto nameStart = KJ_ASSERT_NONNULL(source.find(kj::str(functionName, "(")));
  return static_cast<int>(nameStart + functionName.size());
}

int startPosition(kj::StringPtr functionName) {
  return startPosition(SOURCE, functionName);
}

v8::MaybeLocal<v8::Module> resolveNothing(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> importAttributes,
    v8::Local<v8::Module> referrer) {
  // The test module has no imports.
  return {};
}

v8::Local<v8::Object> getNamespace(jsg::Lock& js, v8::Local<v8::Module> module) {
  return check(module->GetModuleNamespace()->ToObject(js.v8Context()));
}

class HintsObserver final: public CompilationObserver {
 public:
  void onCompileHintsConsumed(
      v8::Isolate* isolate, size_t hintCount, size_t eagerFunctionCount) const override {
    ++calls;
    lastHintCount = hintCount;
    lastEagerFunctionCount = eagerFunctionCount;
    totalEagerFunctionCount += eagerFunctionCount;
  }

  mutable uint calls = 0;
  mutable size_t lastHintCount = 0;
  mutable size_t lastEagerFunctionCount = 0;
  mutable size_t totalEagerFunctionCount = 0;
};

template <typename Func>
void withContext(HintsIsolate& isolate, Func&& func) {
  isolate.runInLockScope([&](HintsIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.newContext<HintsContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) { func(js); });
  });
}

// Compiles SOURCE as a module with kProduceCompileHints, evaluates it, then calls `handler`, as a
// first request would. Returns the positions of the functions V8 compiled lazily meanwhile.
kj::Array<int> produceHints() {
  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  kj::Array<int> result;
  withContext(isolate, [&](jsg::Lock& js) {
    constexpr bool isModule = true;
    v8::ScriptOrigin origin(
        v8StrIntern(js.v8Isolate, "producer.js"_kj), 0, 0, false, -1, {}, false, false, isModule);
    v8::ScriptCompiler::Source source(v8Str(js.v8Isolate, SOURCE), origin);
    auto module = check(v8::ScriptCompiler::CompileModule(
        js.v8Isolate, &source, v8::ScriptCompiler::kProduceCompileHints));
    // Obtained before evaluation; it keeps collecting while the module's functions run.
    auto collector = module->GetUnboundModuleScript()->GetCompileHintsCollector();
    check(module->InstantiateModule(js.v8Context(), &resolveNothing));
    check(module->Evaluate(js.v8Context()));

    auto handler = check(
        getNamespace(js, module)->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "handler"_kj)));
    check(handler.As<v8::Function>()->Call(js.v8Context(), js.v8Undefined(), 0, nullptr));

    auto positions = collector->GetCompileHints(js.v8Isolate);
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
        getNamespace(js, module)->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "result"_kj)));
    KJ_EXPECT(check(result->Int32Value(js.v8Context())) == 42);
  });
}

// Adds lib.js and main.js to the context's module registry, evaluates main.js, and returns its
// namespace.
v8::Local<v8::Object> addAndEvaluateModules(jsg::Lock& js,
    const CompilationObserver& observer,
    kj::ArrayPtr<const int> libHints,
    kj::ArrayPtr<const int> mainHints,
    ProduceCompileHints produceCompileHints) {
  auto registry = ModuleRegistryImpl<HintsIsolate_TypeWrapper>::from(js);
  auto libPath = kj::Path::parse("lib.js");
  registry->add(libPath,
      ModuleRegistry::ModuleInfo(
          js, "lib.js", LIB_SOURCE.asArray(), observer, libHints, produceCompileHints));
  auto mainPath = kj::Path::parse("main.js");
  registry->add(mainPath,
      ModuleRegistry::ModuleInfo(
          js, "main.js", MAIN_SOURCE.asArray(), observer, mainHints, produceCompileHints));

  auto module = KJ_ASSERT_NONNULL(registry->resolve(js, mainPath)).module.getHandle(js);
  instantiateModule(js, module);
  return getNamespace(js, module);
}

void callHandler(jsg::Lock& js, v8::Local<v8::Object> ns) {
  auto handler = check(ns->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "handler"_kj)));
  check(handler.As<v8::Function>()->Call(js.v8Context(), js.v8Undefined(), 0, nullptr));
}

KJ_TEST("the module registry produces hints per module, which make V8 compile eagerly") {
  kj::Array<int> libHints;
  kj::Array<int> mainHints;
  {
    HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
    withContext(isolate, [&](jsg::Lock& js) {
      CompilationObserver observer;
      auto ns = addAndEvaluateModules(js, observer, nullptr, nullptr, ProduceCompileHints::YES);
      // As a first request would.
      callHandler(js, ns);

      auto produced = ModuleRegistry::from(js)->getProducedCompileHints(js);
      KJ_ASSERT(produced.size() == 2, produced.size());
      for (auto& module: produced) {
        if (module.specifier == kj::Path::parse("lib.js")) {
          libHints = kj::mv(module.functionPositions);
        } else if (module.specifier == kj::Path::parse("main.js")) {
          mainHints = kj::mv(module.functionPositions);
        } else {
          KJ_FAIL_EXPECT("unexpected module", module.specifier.toString());
        }
      }
    });
  }

  // `square` ran during evaluation, `cube` never ran.
  KJ_ASSERT(libHints.size() == 1, libHints.size());
  KJ_EXPECT(libHints[0] == startPosition(LIB_SOURCE, "square"));
  // `handler` ran after evaluation, `unused` never ran.
  KJ_ASSERT(mainHints.size() == 1, mainHints.size());
  KJ_EXPECT(mainHints[0] == startPosition(MAIN_SOURCE, "handler"));

  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  withContext(isolate, [&](jsg::Lock& js) {
    HintsObserver observer;
    auto ns = addAndEvaluateModules(js, observer, libHints, mainHints, ProduceCompileHints::NO);
    KJ_EXPECT(observer.calls == 2);
    // `square` and `handler`.
    KJ_EXPECT(observer.totalEagerFunctionCount == 2, observer.totalEagerFunctionCount);
    auto result = check(ns->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "result"_kj)));
    KJ_EXPECT(check(result->Int32Value(js.v8Context())) == 36);
    // Modules compiled without production report nothing.
    KJ_EXPECT(ModuleRegistry::from(js)->getProducedCompileHints(js).size() == 0);
  });
}

KJ_TEST("modules compiled without hint production return no hints") {
  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  withContext(isolate, [&](jsg::Lock& js) {
    CompilationObserver observer;
    ModuleRegistry::ModuleInfo info(js, "normal.js", SOURCE.asArray(), observer);
    auto module = info.module.getHandle(js);
    instantiateModule(js, module);
    callHandler(js, getNamespace(js, module));

    KJ_EXPECT(getProducedCompileHints(js, module).size() == 0);
  });
}

KJ_TEST("producing hints ignores a code cache and hints to consume") {
  auto cache = produceCodeCache();
  const int hints[] = {startPosition("used")};

  HintsIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  withContext(isolate, [&](jsg::Lock& js) {
    HintsObserver observer;
    ModuleRegistry::ModuleInfo info(
        js, "main.js", SOURCE.asArray(), cache, observer, hints, ProduceCompileHints::YES);
    KJ_EXPECT(observer.calls == 0);
    auto module = info.module.getHandle(js);
    check(module->InstantiateModule(js.v8Context(), &resolveNothing));
    check(module->Evaluate(js.v8Context()));
    callHandler(js, getNamespace(js, module));

    auto produced = getProducedCompileHints(js, module);
    KJ_ASSERT(produced.size() == 2, produced.size());
    KJ_EXPECT(produced[0] == startPosition("used"));
    KJ_EXPECT(produced[1] == startPosition("handler"));
  });
}

KJ_TEST("hints produced by a module make V8 compile those functions with the module") {
  auto hints = produceHints();
  // `used` ran during evaluation and `handler` afterwards; `unused` never ran.
  KJ_ASSERT(hints.size() == 2, hints.size());
  KJ_EXPECT(hints[0] == startPosition("used"), hints[0], startPosition("used"));
  KJ_EXPECT(hints[1] == startPosition("handler"), hints[1], startPosition("handler"));

  HintsObserver observer;
  compileAndEvaluateModule(nullptr, hints, observer);
  KJ_EXPECT(observer.calls == 1);
  KJ_EXPECT(observer.lastHintCount == 2);
  // V8 asked about all three functions and compiled the two hinted ones eagerly.
  KJ_EXPECT(observer.lastEagerFunctionCount == 2, observer.lastEagerFunctionCount);
}

KJ_TEST("unsorted hints and positions that start no function are tolerated") {
  // Without normalization, binary search skips `used` after comparing the smaller midpoint.
  const int hints[] = {startPosition("used"), 3, 9999};

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
  const int hints[] = {startPosition("used")};

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
