#pragma once

#include <pyodide/pyodide.capnp.h>
#include <kj/debug.h>

namespace workerd {

namespace _ {
kj::StringPtr lookupModule(kj::StringPtr name) {
  for (auto m : PYODIDE_BUNDLE->getModules()) {
    if (m.getName() == name) {
      return m.getSrc().asChars().begin();
    }
  }
  KJ_UNREACHABLE;
}
}

kj::StringPtr getPyodideBootstrap() {
  return _::lookupModule("pyodide-internal:pyodide-bootstrap");
}

kj::StringPtr getPyodideLock() {
  return _::lookupModule("pyodide-internal:pyodide-lock");
}

}  // namespace workerd
