#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::pyodide {

class DynEvalImpl final: public jsg::Object {
public:
  void enableEval(jsg::Lock& js) {
    js.setAllowEval(true);
  }

  void disableEval(jsg::Lock& js) {
    js.setAllowEval(false);
  }

  JSG_RESOURCE_TYPE(DynEvalImpl) {
    JSG_METHOD(enableEval);
    JSG_METHOD(disableEval);
  }
};

#define EW_EVAL_ISOLATE_TYPES                    \
    api::pyodide::DynEvalImpl

}
