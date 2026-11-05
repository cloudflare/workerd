#include "script.h"

#include <workerd/jsg/compilation.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/setup.h>

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
  v8::ScriptOrigin origin(js.str(name));
  return NonModuleScript(js,
      check(compileScript(js, code, origin, ScriptCompilationMode::INTERNAL,
          IsolateBase::from(js.v8Isolate).getObserver())));
}

NonModuleScript NonModuleScript::compileWorkerScript(
    jsg::Lock& js, kj::StringPtr code, kj::StringPtr name) {
  v8::ScriptOrigin origin(js.str(name));
  return NonModuleScript(js,
      check(compileScript(js, code, origin, ScriptCompilationMode::WORKER,
          IsolateBase::from(js.v8Isolate).getObserver())));
}

}  // namespace workerd::jsg
