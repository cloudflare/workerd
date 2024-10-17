#include "setup-emscripten.h"

#include <pyodide/generated/emscripten_setup.capnp.h>

namespace workerd::api::pyodide {

jsg::JsValue SetupEmscripten::getModule(jsg::Lock& js) {
  KJ_IF_SOME(module, emscriptenModule) {
    return module.getHandle(js);
  } else {
    auto& runtime = KJ_ASSERT_NONNULL(Worker::Api::current().getEmscriptenRuntime());
    js.v8Context()->SetSecurityToken(runtime.contextToken.getHandle(js));
    emscriptenModule = runtime.emscriptenRuntime;
    return KJ_ASSERT_NONNULL(emscriptenModule).getHandle(js);
  }
}

void SetupEmscripten::visitForGc(jsg::GcVisitor& visitor) {
  // const_cast is ok because the GcVisitor doesn't actually change the underlying value of the object.
  KJ_IF_SOME(module, emscriptenModule) {
    visitor.visit(const_cast<jsg::JsRef<jsg::JsValue>&>(module));
  }
}

v8::Local<v8::Module> loadEmscriptenSetupModule(jsg::Lock& js) {
  v8::Local<v8::String> contentStr =
      jsg::v8Str(js.v8Isolate, EMSCRIPTEN_SETUP->getCode().asArray());
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

void handleLog(jsg::Lock& js,
    LogLevel level,
    const v8::Global<v8::Function>& original,
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  // Call original V8 implementation so messages sent to connected inspector if any
  auto context = js.v8Context();
  int length = info.Length();
  v8::LocalVector<v8::Value> args(js.v8Isolate, length + 1);
  for (auto i: kj::zeroTo(length)) args[i] = info[i];
  jsg::check(original.Get(js.v8Isolate)->Call(context, info.This(), length, args.data()));

  // The TryCatch is initialised here to catch cases where the v8 isolate's execution is
  // terminating, usually as a result of an infinite loop. We need to perform the initialisation
  // here because `message` is called multiple times.
  v8::TryCatch tryCatch(js.v8Isolate);
  auto message = [&]() {
    int length = info.Length();
    kj::Vector<kj::String> stringified(length);
    for (auto i: kj::zeroTo(length)) {
      auto arg = info[i];
      // serializeJson and v8::Value::ToString can throw JS exceptions
      // (e.g. for recursive objects) so we eat them here, to ensure logging and non-logging code
      // have the same exception behavior.
      if (!tryCatch.CanContinue()) {
        stringified.add(kj::str("{}"));
        break;
      }
      // The following code checks the `arg` to see if it should be serialised to JSON.
      //
      // We use the following criteria: if arg is null, a number, a boolean, an array, a string, an
      // object or it defines a `toJSON` property that is a function, then the arg gets serialised
      // to JSON.
      //
      // Otherwise we stringify the argument.
      js.withinHandleScope([&] {
        auto context = js.v8Context();
        bool shouldSerialiseToJson = false;
        if (arg->IsNull() || arg->IsNumber() || arg->IsArray() || arg->IsBoolean() ||
            arg->IsString() ||
            arg->IsUndefined()) {  // This is special cased for backwards compatibility.
          shouldSerialiseToJson = true;
        }
        if (arg->IsObject()) {
          v8::Local<v8::Object> obj = arg.As<v8::Object>();
          v8::Local<v8::Object> freshObj = v8::Object::New(js.v8Isolate);

          // Determine whether `obj` is constructed using `{}` or `new Object()`. This ensures
          // we don't serialise values like Promises to JSON.
          if (obj->GetPrototypeV2()->SameValue(freshObj->GetPrototypeV2()) ||
              obj->GetPrototypeV2()->IsNull()) {
            shouldSerialiseToJson = true;
          }

          // Check if arg has a `toJSON` property which is a function.
          auto toJSONStr = jsg::v8StrIntern(js.v8Isolate, "toJSON"_kj);
          v8::MaybeLocal<v8::Value> toJSON = obj->GetRealNamedProperty(context, toJSONStr);
          if (!toJSON.IsEmpty()) {
            if (jsg::check(toJSON)->IsFunction()) {
              shouldSerialiseToJson = true;
            }
          }
        }

        if (kj::runCatchingExceptions([&]() {
          // On the off chance the the arg is the request.cf object, let's make
          // sure we do not log proxied fields here.
          if (shouldSerialiseToJson) {
            auto s = js.serializeJson(arg);
            // serializeJson returns the string "undefined" for some values (undefined,
            // Symbols, functions).  We remap these values to null to ensure valid JSON output.
            if (s == "undefined"_kj) {
              stringified.add(kj::str("null"));
            } else {
              stringified.add(kj::mv(s));
            }
          } else {
            stringified.add(js.serializeJson(jsg::check(arg->ToString(context))));
          }
        }) != kj::none) {
          stringified.add(kj::str("{}"));
        };
      });
    }
    return kj::str("[", kj::delimited(stringified, ", "_kj), "]");
  };

  KJ_LOG(INFO, "console.log()", message());
}

void setupConsole(jsg::Lock& lock, v8::Local<v8::Object>& global, v8::Local<v8::Context>& context) {
  auto consoleStr = jsg::v8StrIntern(lock.v8Isolate, "console");
  auto console = jsg::check(global->Get(context, consoleStr)).As<v8::Object>();
  auto setHandler = [&](const char* method, LogLevel level) {
    auto methodStr = jsg::v8StrIntern(lock.v8Isolate, method);
    v8::Global<v8::Function> original(
        lock.v8Isolate, jsg::check(console->Get(context, methodStr)).As<v8::Function>());

    auto f = lock.wrapSimpleFunction(context,
        [level, original = kj::mv(original)](
            jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
      handleLog(js, level, original, info);
    });
    jsg::check(console->Set(context, methodStr, f));
  };

  setHandler("debug", LogLevel::DEBUG_);
  setHandler("error", LogLevel::ERROR);
  setHandler("info", LogLevel::INFO);
  setHandler("log", LogLevel::LOG);
  setHandler("warn", LogLevel::WARN);
}
void setWebAssemblyModuleHasInstance(jsg::Lock& lock, v8::Local<v8::Context> context) {
  auto instanceof = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    jsg::Lock::from(info.GetIsolate()).withinHandleScope([&] {
      info.GetReturnValue().Set(info[0]->IsWasmModuleObject());
    });
  };
  v8::Local<v8::Function> function = jsg::check(v8::Function::New(context, instanceof));

  v8::Object* webAssembly = v8::Object::Cast(*jsg::check(
      context->Global()->Get(context, jsg::v8StrIntern(lock.v8Isolate, "WebAssembly"))));
  v8::Object* module = v8::Object::Cast(
      *jsg::check(webAssembly->Get(context, jsg::v8StrIntern(lock.v8Isolate, "Module"))));

  jsg::check(
      module->DefineOwnProperty(context, v8::Symbol::GetHasInstance(lock.v8Isolate), function));
}

EmscriptenRuntime initializeEmscriptenRuntime(jsg::Lock& js, bool isWorkerd) {
  // TODO: add tracing span
  auto context = js.v8Context();
  auto global = context->Global();
  setWebAssemblyModuleHasInstance(js, context);
  setupConsole(js, global, context);
  auto module = loadEmscriptenSetupModule(js);
  instantiateEmscriptenSetupModule(js, module);
  auto instantiateEmscriptenModule = getInstantiateEmscriptenModule(js, module);
  auto emscriptenModule = callInstantiateEmscriptenModule(js, instantiateEmscriptenModule,
      isWorkerd, EMSCRIPTEN_SETUP->getPythonStdlibZip(), EMSCRIPTEN_SETUP->getPyodideAsmWasm());
  auto contextToken = jsg::JsValue(context->GetSecurityToken());
  return EmscriptenRuntime{contextToken.addRef(js), emscriptenModule.addRef(js)};
}
}  // namespace workerd::api::pyodide
