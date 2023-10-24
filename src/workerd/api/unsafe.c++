#include "unsafe.h"

namespace workerd::api {

namespace {
static constexpr auto EVAL_STR = "eval"_kjc;
inline kj::StringPtr getName(jsg::Optional<kj::String>& name, kj::StringPtr def) {
  return name.map([](kj::String& str) {
    return str.asPtr();
  }).orDefault(def);
}
}  // namespace

jsg::JsValue UnsafeEval::eval(jsg::Lock& js, kj::String script,
                              jsg::Optional<kj::String> name) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));
  auto compiled = jsg::NonModuleScript::compile(script, js, getName(name, EVAL_STR));
  return jsg::JsValue(compiled.runAndReturn(js.v8Context()));
}

UnsafeEval::UnsafeEvalFunction UnsafeEval::newFunction(
    jsg::Lock& js,
    jsg::JsString script,
    jsg::Optional<kj::String> name,
    jsg::Arguments<jsg::JsRef<jsg::JsString>> args,
    const jsg::TypeHandler<UnsafeEvalFunction>& handler) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  auto nameStr = js.str(getName(name, EVAL_STR));
  v8::ScriptOrigin origin(js.v8Isolate, nameStr);
  v8::ScriptCompiler::Source source(script, origin);

  auto argNames = KJ_MAP(arg, args) {
    return v8::Local<v8::String>(arg.getHandle(js));
  };

  auto fn = jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &source, argNames.size(), argNames.begin(), 0, nullptr));
  fn->SetName(nameStr);

  return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, fn));
}

}  // namespace workerd::api
