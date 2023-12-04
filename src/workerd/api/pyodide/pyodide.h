#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/api/pyodide/eval.h>

namespace workerd::api::pyodide {

template <class Registry>
void registerPyodideModules(
    Registry& registry, auto featureFlags) {

  registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  registry.template addBuiltinModule<DynEvalImpl>("pyodide-internal:eval", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

}
