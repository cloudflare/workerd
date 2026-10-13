// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/setup.h>
#include <workerd/jsg/type-wrapper.h>

// API declarations need the JSG implementation templates above for their JSG_STRUCT expansions.
#include "pyodide.h"

#include <workerd/io/worker-modules.h>

#include <capnp/serialize.h>
#include <kj/test.h>

namespace workerd::api::pyodide {
namespace {

struct PythonContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(PythonContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(PythonIsolate, PythonContext, EW_PYODIDE_ISOLATE_TYPES);

class WasmCompilationObserver final: public jsg::CompilationObserver {
 public:
  mutable uint compiled = 0;
  mutable uint fromCache = 0;

  kj::Own<void> onWasmCompilationStart(v8::Isolate*, size_t) const override {
    ++compiled;
    return {};
  }

  kj::Own<void> onWasmCompilationFromCacheStart(v8::Isolate*) const override {
    ++fromCache;
    return {};
  }
};

WD_STRONG_BOOL(WithWasmBuiltins);

KJ_TEST("Python module registration reuses manager-owned Wasm after isolate destruction") {
  jsg::V8System v8System;
  PyodideBundleManager manager;
  capnp::MallocMessageBuilder message;
  auto wasm = message.initRoot<jsg::Bundle>().initModules(1)[0];
  wasm.setName("pyodide-internal:pyodide.asm.wasm");
  wasm.setType(jsg::ModuleRegistry::Type::INTERNAL);
  static constexpr kj::byte WIRE_BYTES[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
  wasm.setWasm(kj::arrayPtr(WIRE_BYTES, kj::size(WIRE_BYTES)));
  auto serialized = capnp::messageToFlatArray(message);
  manager.setPyodideBundleData(kj::str("dev"), kj::heapArray(serialized.asBytes()));

  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setPythonWorkers(true);
  flags.setPythonWorkersDevPyodide(true);
  WorkerSource::ModulesSource source{
    .mainModule = "main.py",
    .modules = kj::arr(WorkerSource::Module{
      .name = "main.py", .content = WorkerSource::PythonModule{.body = "pass\n"}}),
    .isPython = true,
  };
  WasmCompilationObserver observer;

  // Each call destroys its registry and isolate before returning. The manager is the only owner
  // retaining compiled Wasm between calls, and registration must connect its holder to the module.
  auto load = [&](WithWasmBuiltins withBuiltins) {
    PythonIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
    isolate.runInLockScope([&](PythonIsolate::Lock& js) {
      JSG_WITHIN_CONTEXT_SCOPE(
          js, js.newContext<PythonContext>().getHandle(js), [&](jsg::Lock& js) {
        if (withBuiltins) js.setWasmEsmIntegrationBuiltinsEnabled();
        auto registry = jsg::ModuleRegistryImpl<PythonIsolate_TypeWrapper>::install(
            js.v8Isolate, js.v8Context(), observer);
        modules::python::registerPythonCommonModules<DisabledInternalJaeger>(js, *registry,
            flags.asReader(), KJ_REQUIRE_NONNULL(manager.getPyodideBundle("dev")), source, kj::none,
            IsWorkerd::YES, IsTracing::NO, SnapshotToDisk::NO, CreateBaselineSnapshot::NO, kj::none,
            kj::none, kj::none, kj::none, manager.getPyodideWasmModuleHolder("dev"));
        auto path = kj::Path::parse("pyodide-internal:pyodide.asm.wasm");
        auto& info = KJ_REQUIRE_NONNULL(registry->resolve(
            js, path, kj::none, jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY));
        auto module = info.module.getHandle(js);
        jsg::instantiateModule(js, module);
        auto exports = jsg::check(module->GetModuleNamespace()->ToObject(js.v8Context()));
        auto value =
            jsg::check(exports->Get(js.v8Context(), jsg::v8StrIntern(js.v8Isolate, "default"_kj)));
        KJ_ASSERT(value->IsWasmModuleObject());
      });
    });
  };

  load(WithWasmBuiltins::NO);
  KJ_ASSERT(observer.compiled == 1);
  KJ_ASSERT(observer.fromCache == 0);
  load(WithWasmBuiltins::NO);
  KJ_EXPECT(observer.compiled == 1, observer.compiled);
  KJ_EXPECT(observer.fromCache == 1, observer.fromCache);

  load(WithWasmBuiltins::YES);
  KJ_EXPECT(observer.compiled == 2, observer.compiled);
  KJ_EXPECT(observer.fromCache == 1, observer.fromCache);
  load(WithWasmBuiltins::YES);
  KJ_EXPECT(observer.compiled == 2, observer.compiled);
  KJ_EXPECT(observer.fromCache == 2, observer.fromCache);
}

}  // namespace
}  // namespace workerd::api::pyodide
