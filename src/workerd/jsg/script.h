#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::jsg {

// jsg::NonModuleScript wraps a v8::UnboundScript.
// An unbound script is a script that has been compiled but is not
// yet bound to a specific context.
class NonModuleScript final {
 public:
  NonModuleScript(jsg::Lock& js, v8::Local<v8::UnboundScript> script)
      : unboundScript(js.v8Isolate, script) {}

  NonModuleScript(NonModuleScript&&) = default;
  NonModuleScript& operator=(NonModuleScript&&) = default;
  KJ_DISALLOW_COPY(NonModuleScript);

  // Running the script will create a v8::Script instance bound to the given
  // context then will run it to completion.
  void run(jsg::Lock& js) const;

  jsg::JsValue runAndReturn(jsg::Lock& js) const;

  static jsg::NonModuleScript compile(
      jsg::Lock& js, kj::StringPtr code, kj::StringPtr name = "worker.js");

 private:
  v8::Global<v8::UnboundScript> unboundScript;
};

}  // namespace workerd::jsg
