#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>

namespace workerd::api::pyodide {

template <class Registry>
void registerPyodideModules(
    Registry& registry, auto featureFlags) {

  registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
}

}
