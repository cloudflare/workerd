#include "script.h"

namespace workerd::jsg {

v8::Local<v8::Value> NonModuleScript::runAndReturn(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.getHandle(isolate)->BindToCurrentContext();
  return check(boundScript->Run(context));
}

void NonModuleScript::run(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.getHandle(isolate)->BindToCurrentContext();
  check(boundScript->Run(context));
}

NonModuleScript NonModuleScript::compile(jsg::Lock& js, kj::StringPtr code, kj::StringPtr name) {
  // Create a dummy script origin for it to appear in Sources panel.
  auto isolate = js.v8Isolate;
  v8::ScriptOrigin origin(js.str(name));
  v8::ScriptCompiler::Source source(js.str(code), origin);
  return NonModuleScript(js, check(v8::ScriptCompiler::CompileUnboundScript(isolate, &source)));
}

}  // namespace workerd::jsg
