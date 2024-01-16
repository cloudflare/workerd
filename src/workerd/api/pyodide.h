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

kj::StringPtr getPyodidePatch(kj::StringPtr name) {
  return _::lookupModule(kj::str("pyodide:internal/patches/", name));
}

capnp::Data::Reader getPyodideEmbeddedPackages() {
  // TODO(later): strip the version from this.
  auto moduleName = "pyodide:generated/pyodide_packages_unzipped_0.1.tar";
  for (auto m : PYODIDE_BUNDLE->getModules()) {
    if (m.getName() == moduleName) {
      return m.getSrc();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd
