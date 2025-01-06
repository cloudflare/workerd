#include "unsafe.h"

#include <workerd/jsg/script.h>

namespace workerd::api {

namespace {
static constexpr auto EVAL_STR = "eval"_kjc;
static constexpr auto ANON_STR = "anonymous"_kjc;
static constexpr auto ASYNC_FN_PREFIX = "async function "_kjc;
static constexpr auto ASYNC_FN_ARG_OPEN = "("_kjc;
static constexpr auto ASYNC_FN_ARG_CLOSE = ") {"_kjc;
static constexpr auto ASYNC_FN_SUFFIX = "}"_kjc;

inline kj::StringPtr getName(jsg::Optional<kj::String>& name, kj::StringPtr def) {
  return name.map([](kj::String& str) { return str.asPtr(); }).orDefault(def);
}
}  // namespace

jsg::JsValue UnsafeEval::eval(jsg::Lock& js, kj::String script, jsg::Optional<kj::String> name) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));
  auto compiled = jsg::NonModuleScript::compile(js, script, getName(name, EVAL_STR));
  return compiled.runAndReturn(js);
}

UnsafeEval::UnsafeEvalFunction UnsafeEval::newFunction(jsg::Lock& js,
    jsg::JsString script,
    jsg::Optional<kj::String> name,
    jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
    const jsg::TypeHandler<UnsafeEvalFunction>& handler) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto nameStr = js.str(getName(name, ANON_STR));
  v8::ScriptOrigin origin(nameStr);
  v8::ScriptCompiler::Source source(script, origin);

  auto argNames = KJ_MAP(arg, args) { return v8::Local<v8::String>(arg.getHandle(js)); };

  auto fn = jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &source, argNames.size(), argNames.begin(), 0, nullptr));
  fn->SetName(nameStr);

  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, fn));
}

UnsafeEval::UnsafeEvalFunction UnsafeEval::newAsyncFunction(jsg::Lock& js,
    jsg::JsString script,
    jsg::Optional<kj::String> name,
    jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
    const jsg::TypeHandler<UnsafeEvalFunction>& handler) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto nameStr = js.str(getName(name, ANON_STR));

  // This case is sadly a bit more complicated than the newFunction variant
  // because v8 currently (surprisingly) does not actually expose a way
  // CompileAsyncFunction variant (silly v8). What we end up doing here is
  // building a string that wraps the script provided by the caller:
  //
  //   async function {name}({args}) { {script} }; {name}
  //
  // Where {name} is the name of the function as provided by the user or
  // "anonymous" by default, {args} is the list of args provided by the
  // caller, if any. We end the constructed string with the name of the
  // function so that the result of running the compiled script is a reference
  // to the compiled function.

  auto prepared = v8::String::Concat(js.v8Isolate, js.strIntern(ASYNC_FN_PREFIX), nameStr);
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_ARG_OPEN));

  for (auto& arg: args) {
    prepared = v8::String::Concat(js.v8Isolate, prepared, arg.getHandle(js));
    prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(","));
  }
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_ARG_CLOSE));
  prepared = v8::String::Concat(js.v8Isolate, prepared, script);
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(ASYNC_FN_SUFFIX));
  prepared = v8::String::Concat(js.v8Isolate, prepared, js.strIntern(";"));
  prepared = v8::String::Concat(js.v8Isolate, prepared, nameStr);

  v8::ScriptOrigin origin(nameStr);
  v8::ScriptCompiler::Source source(prepared, origin);

  auto compiled = jsg::check(v8::ScriptCompiler::Compile(js.v8Context(), &source));
  auto result = jsg::check(compiled->Run(js.v8Context()));

  KJ_REQUIRE(result->IsAsyncFunction());

  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, result.As<v8::Function>()));
}

jsg::JsValue UnsafeEval::newWasmModule(jsg::Lock& js, kj::Array<kj::byte> src) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto maybeWasmModule = v8::WasmModuleObject::Compile(
      js.v8Isolate, v8::MemorySpan<const uint8_t>(src.begin(), src.size()));
  return jsg::JsValue(jsg::check(maybeWasmModule));
}

jsg::Promise<void> UnsafeModule::abortAllDurableObjects(jsg::Lock& js) {
  auto& context = IoContext::current();
  // Abort all actors asynchronously to avoid recursively taking isolate lock in actor destructor
  auto promise = kj::evalLater([&]() { return context.abortAllActors(); });
  return context.awaitIo(js, kj::mv(promise));
}

}  // namespace workerd::api
