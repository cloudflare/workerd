#include "setup-emscripten.h"

#include <pyodide/generated/emscripten_setup.capnp.h>

namespace workerd::api::pyodide {

jsg::JsValue SetupEmscripten::getModule(jsg::Lock& js) {
  jsg::JsValue jsval(pyodide::initializeEmscriptenRuntime(js));
  module_ = jsval.addRef(js);
  return KJ_ASSERT_NONNULL(module_).getHandle(js);
}

v8::Local<v8::Module> loadEmscriptenSetupModule(jsg::Lock& js) {
  v8::Local<v8::String> contentStr =
      jsg::v8Str(js.v8Isolate, EMSCRIPTEN_SETUP->getCode().asArray());
  v8::ScriptOrigin origin(
      jsg::v8StrIntern(js.v8Isolate, "pyodide-internal:generated/emscriptenSetup"), 0, 0, false, -1,
      v8::Local<v8::Value>(), false, false, true);
  v8::ScriptCompiler::Source source(contentStr, origin);
  return jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
}
void instantiateEmscriptenSetupModule(jsg::Lock& js, v8::Local<v8::Module>& module) {
  jsg::instantiateModule(js, module);
  auto handle = jsg::check(module->Evaluate(js.v8Context()));
  KJ_ASSERT(handle->IsPromise());
  auto prom = handle.As<v8::Promise>();
  KJ_ASSERT(prom->State() != v8::Promise::PromiseState::kPending);
  KJ_ASSERT(module->GetStatus() != v8::Module::kErrored);
}

v8::Local<v8::Function> getInstantiateEmscriptenModule(
    jsg::Lock& js, v8::Local<v8::Module>& module) {
  auto instantiateEmscriptenModule =
      js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "instantiateEmscriptenModule"_kj);
  KJ_ASSERT(instantiateEmscriptenModule->IsFunction());
  return instantiateEmscriptenModule.As<v8::Function>();
}

v8::Local<v8::Value> callInstantiateEmscriptenModule(jsg::Lock& js,
    v8::Local<v8::Function>& func,
    bool isWorkerd,
    capnp::Data::Reader pythonStdlibZipReader,
    capnp::Data::Reader pyodideAsmWasmReader) {

  auto pythonStdlibZip = v8::ArrayBuffer::New(js.v8Isolate, pythonStdlibZipReader.size(),
      v8::BackingStoreInitializationMode::kUninitialized);
  memcpy(pythonStdlibZip->Data(), pythonStdlibZipReader.begin(), pythonStdlibZipReader.size());
  auto pyodideAsmWasm = jsg::check(v8::WasmModuleObject::Compile(js.v8Isolate,
      v8::MemorySpan<const uint8_t>(pyodideAsmWasmReader.begin(), pyodideAsmWasmReader.size())));

  v8::LocalVector<v8::Value> argv(js.v8Isolate, 3);
  argv[0] = v8::Boolean::New(js.v8Isolate, true);
  argv[1] = kj::mv(pythonStdlibZip);
  argv[2] = kj::mv(pyodideAsmWasm);
  auto funcres = jsg::check(func->Call(js.v8Context(), js.v8Null(), argv.size(), argv.data()));
  KJ_ASSERT(funcres->IsPromise());
  auto promise = funcres.As<v8::Promise>();
  if (promise->State() == v8::Promise::PromiseState::kPending) {
    js.runMicrotasks();
  }
  KJ_ASSERT(promise->State() == v8::Promise::PromiseState::kFulfilled);
  return promise->Result();
}

v8::Local<v8::Value> initializeEmscriptenRuntime(jsg::Lock& js) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));
  auto module = loadEmscriptenSetupModule(js);
  instantiateEmscriptenSetupModule(js, module);
  auto instantiateEmscriptenModule = getInstantiateEmscriptenModule(js, module);
  return callInstantiateEmscriptenModule(js, instantiateEmscriptenModule, true,
      EMSCRIPTEN_SETUP->getPythonStdlibZip(), EMSCRIPTEN_SETUP->getPyodideAsmWasm());
}
}  // namespace workerd::api::pyodide
