#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/api/pyodide/eval.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (!util::Autogate::isEnabled(util::AutogateKey::BUILTIN_WASM_MODULES)) {
    return;
  }
  registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  registry.template addBuiltinModule<DynEvalImpl>("pyodide-internal:eval",
                                                  workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

} // namespace workerd::api::pyodide
