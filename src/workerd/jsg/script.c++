#include "script.h"

namespace workerd::jsg {

jsg::JsValue NonModuleScript::runAndReturn(jsg::Lock& js) const {
  auto boundScript = unboundScript.Get(js.v8Isolate)->BindToCurrentContext();
  return jsg::JsValue(check(boundScript->Run(js.v8Context())));
}

void NonModuleScript::run(jsg::Lock& js) const {
  auto boundScript = unboundScript.Get(js.v8Isolate)->BindToCurrentContext();
  check(boundScript->Run(js.v8Context()));
}

NonModuleScript NonModuleScript::compile(jsg::Lock& js, kj::StringPtr code, kj::StringPtr name) {
  // Create a dummy script origin for it to appear in Sources panel.
  auto isolate = js.v8Isolate;
  v8::ScriptOrigin origin(js.str(name));
  v8::ScriptCompiler::Source source(js.str(code), origin);
  return NonModuleScript(js, check(v8::ScriptCompiler::CompileUnboundScript(isolate, &source)));
}

}  // namespace workerd::jsg
