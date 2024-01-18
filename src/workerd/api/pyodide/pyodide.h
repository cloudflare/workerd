#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <pyodide/generated/pyodide_packages.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

// A special binding object that allows for dynamic evaluation.
class PackagesTarReader: public jsg::Object {
public:
  PackagesTarReader() = default;

  void read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
    auto x = *PYODIDE_PACKAGES_TAR;
    kj::byte* start = buf.begin();
    for (int i = 0; i < buf.size(); i++) {
      start[i] = x[offset + i];
    }
  }

  JSG_RESOURCE_TYPE(PackagesTarReader) {
    JSG_METHOD(read);
  }
};


#define EW_PYODIDE_ISOLATE_TYPES api::pyodide::PackagesTarReader

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getWorkerdExperimental() &&
      util::Autogate::isEnabled(util::AutogateKey::BUILTIN_WASM_MODULES)) {
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  }
  registry.template addBuiltinModule<PackagesTarReader>("pyodide-internal:packages_tar_reader",
                                             workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

} // namespace workerd::api::pyodide
