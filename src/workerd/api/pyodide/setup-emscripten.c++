#include "setup-emscripten.h"

#include <workerd/io/trace.h>
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

v8::Local<v8::Function> getInstantiateEmscriptenModule(
    jsg::Lock& js, v8::Local<v8::Module>& module) {
  auto instantiateEmscriptenModule =
      js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "instantiateEmscriptenModule"_kj);
  KJ_ASSERT(instantiateEmscriptenModule->IsFunction());
  return instantiateEmscriptenModule.As<v8::Function>();
}

template <typename... Args>
jsg::JsValue callFunction(jsg::Lock& js, v8::Local<v8::Function>& func, Args... args) {
  v8::LocalVector<v8::Value> argv(
      js.v8Isolate, std::initializer_list<v8::Local<v8::Value>>{args...});
  return jsg::JsValue(
      jsg::check(func->Call(js.v8Context(), js.v8Null(), argv.size(), argv.data())));
}

jsg::JsValue callInstantiateEmscriptenModule(jsg::Lock& js,
    v8::Local<v8::Function>& func,
    bool isWorkerd,
    capnp::Data::Reader pythonStdlibZipReader,
    capnp::Data::Reader pyodideAsmWasmReader) {
  AllowV8BackgroundThreadsScope scope;
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto pythonStdlibZip = v8::ArrayBuffer::New(js.v8Isolate, pythonStdlibZipReader.size(),
      v8::BackingStoreInitializationMode::kUninitialized);
  memcpy(pythonStdlibZip->Data(), pythonStdlibZipReader.begin(), pythonStdlibZipReader.size());
  auto pyodideAsmWasm = jsg::check(v8::WasmModuleObject::Compile(js.v8Isolate,
      v8::MemorySpan<const uint8_t>(pyodideAsmWasmReader.begin(), pyodideAsmWasmReader.size())));
  return resolvePromise(js,
      callFunction(
          js, func, js.boolean(isWorkerd), kj::mv(pythonStdlibZip), kj::mv(pyodideAsmWasm)));
}

EmscriptenRuntime EmscriptenRuntime::initialize(
    jsg::Lock& js, bool isWorkerd, jsg::Bundle::Reader bundle) {
  kj::Maybe<capnp::Data::Reader> emsciptenSetupJsReader;
  kj::Maybe<capnp::Data::Reader> pythonStdlibZipReader;
  kj::Maybe<capnp::Data::Reader> pyodideAsmWasmReader;
  for (auto module: bundle.getModules()) {
    if (module.getName().endsWith("emscriptenSetup.js")) {
      emsciptenSetupJsReader = module.getData();
    } else if (module.getName().endsWith("python_stdlib.zip")) {
      pythonStdlibZipReader = module.getData();
    } else if (module.getName().endsWith("pyodide.asm.wasm")) {
      pyodideAsmWasmReader = module.getData();
    }
  }
  auto context = js.v8Context();
  Worker::setupContext(js, context, Worker::ConsoleMode::INSPECTOR_ONLY);
  auto module = loadEmscriptenSetupModule(js, KJ_ASSERT_NONNULL(emsciptenSetupJsReader));
  instantiateEmscriptenSetupModule(js, module);
  auto instantiateEmscriptenModule = getInstantiateEmscriptenModule(js, module);
  auto emscriptenModule = callInstantiateEmscriptenModule(js, instantiateEmscriptenModule,
      isWorkerd, KJ_ASSERT_NONNULL(pythonStdlibZipReader), KJ_ASSERT_NONNULL(pyodideAsmWasmReader));
  auto contextToken = jsg::JsValue(context->GetSecurityToken());
  return EmscriptenRuntime{contextToken.addRef(js), emscriptenModule.addRef(js)};
}
}  // namespace workerd::api::pyodide
