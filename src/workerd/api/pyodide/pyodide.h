#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (!util::Autogate::isEnabled(util::AutogateKey::BUILTIN_WASM_MODULES)) {
    return;
  }
  registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
}

} // namespace workerd::api::pyodide
