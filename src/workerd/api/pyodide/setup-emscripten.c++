#include "setup-emscripten.h"

#include <workerd/io/worker.h>

namespace workerd::api::pyodide {

v8::Local<v8::Module> loadEmscriptenSetupModule(
    jsg::Lock& js, capnp::Data::Reader emsciptenSetupJsReader) {
  v8::Local<v8::String> contentStr = jsg::v8Str(js.v8Isolate, emsciptenSetupJsReader.asChars());
  v8::ScriptOrigin origin(
      jsg::v8StrIntern(js.v8Isolate, "pyodide-internal:generated/emscriptenSetup"), 0, 0, false, -1,
      {}, false, false, true);
  v8::ScriptCompiler::Source source(contentStr, origin);
  return jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
}

jsg::JsValue resolvePromise(jsg::Lock& js, jsg::JsValue prom) {
  auto promise = KJ_ASSERT_NONNULL(prom.tryCast<jsg::JsPromise>());
  if (promise.state() == jsg::PromiseState::PENDING) {
    js.runMicrotasks();
  }
  KJ_ASSERT(promise.state() == jsg::PromiseState::FULFILLED);
  return promise.result();
}

void instantiateEmscriptenSetupModule(jsg::Lock& js, v8::Local<v8::Module>& module) {
  jsg::instantiateModule(js, module);
  auto evalPromise = KJ_ASSERT_NONNULL(
      jsg::JsValue(jsg::check(module->Evaluate(js.v8Context()))).tryCast<jsg::JsPromise>());
  resolvePromise(js, evalPromise);
  KJ_ASSERT(module->GetStatus() == v8::Module::kEvaluated);
}

jsg::JsFunction getInstantiateEmscriptenModule(jsg::Lock& js, v8::Local<v8::Module>& module) {
  auto instantiateEmscriptenModule =
      js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "instantiateEmscriptenModule"_kj);
  KJ_ASSERT(instantiateEmscriptenModule->IsFunction());
  return jsg::JsFunction(instantiateEmscriptenModule.As<v8::Function>());
}

jsg::JsValue callInstantiateEmscriptenModule(jsg::Lock& js,
    const jsg::JsFunction& func,
    bool isWorkerd,
    capnp::Data::Reader pythonStdlibZipReader,
    capnp::Data::Reader pyodideAsmWasmReader) {
  AllowV8BackgroundThreadsScope scope;
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto backingStore =
      js.allocBackingStore(pythonStdlibZipReader.size(), jsg::Lock::AllocOption::UNINITIALIZED);
  auto pythonStdlibZip = v8::ArrayBuffer::New(js.v8Isolate, kj::mv(backingStore));
  memcpy(pythonStdlibZip->Data(), pythonStdlibZipReader.begin(), pythonStdlibZipReader.size());
  auto pyodideAsmWasm = jsg::check(v8::WasmModuleObject::Compile(js.v8Isolate,
      v8::MemorySpan<const uint8_t>(pyodideAsmWasmReader.begin(), pyodideAsmWasmReader.size())));
  return resolvePromise(js,
      func.call(js, js.null(), js.boolean(isWorkerd), jsg::JsValue(pythonStdlibZip),
          jsg::JsValue(pyodideAsmWasm)));
}

EmscriptenRuntime EmscriptenRuntime::initialize(
    jsg::Lock& js, bool isWorkerd, jsg::Bundle::Reader bundle) {
  kj::Maybe<capnp::Data::Reader> emsciptenSetupJsReader;
  kj::Maybe<capnp::Data::Reader> pythonStdlibZipReader;
  kj::Maybe<capnp::Data::Reader> pyodideAsmWasmReader;
#if V8_MAJOR_VERSION < 14 || V8_MINOR_VERSION < 2
  // JSPI was stabilized in V8 version 14.2, and this API removed.
  // TODO(cleanup): Remove this when workerd's V8 version is updated to 14.2.
  js.installJspi();
#endif
  for (auto module: bundle.getModules()) {
    if (module.getName().endsWith("emscriptenSetup.js")) {
      emsciptenSetupJsReader = module.getData();
    } else if (module.getName().endsWith("python_stdlib.zip")) {
      pythonStdlibZipReader = module.getData();
    } else if (module.getName().endsWith("pyodide.asm.wasm")) {
      pyodideAsmWasmReader = module.getData();
    }
  }
  auto module = loadEmscriptenSetupModule(js, KJ_ASSERT_NONNULL(emsciptenSetupJsReader));
  instantiateEmscriptenSetupModule(js, module);
  auto instantiateEmscriptenModule = getInstantiateEmscriptenModule(js, module);
  auto emscriptenModule = callInstantiateEmscriptenModule(js, instantiateEmscriptenModule,
      isWorkerd, KJ_ASSERT_NONNULL(pythonStdlibZipReader), KJ_ASSERT_NONNULL(pyodideAsmWasmReader));
  return EmscriptenRuntime{emscriptenModule.addRef(js)};
}
}  // namespace workerd::api::pyodide
