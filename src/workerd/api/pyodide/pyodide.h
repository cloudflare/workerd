#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <pyodide/generated/pyodide_extra.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

// A function to read a segment of the tar file into a buffer
// Set up this way to avoid copying files that aren't accessed.
class PackagesTarReader: public jsg::Object {
public:
  PackagesTarReader() = default;

  void read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
    int tarSize = PYODIDE_PACKAGES_TAR->size();
    if (offset >= tarSize) {
      return;
    }
    int toCopy = buf.size();
    if (tarSize - offset < toCopy) {
      toCopy = tarSize - offset;
    }
    memcpy(buf.begin(), &((*PYODIDE_PACKAGES_TAR)[0]) + offset, toCopy);
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
