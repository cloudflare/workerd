#pragma once

#include <kj/common.h>
#include <pyodide/pyodide.capnp.h>
#include <pyodide/generated/packages-tar.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

// A special binding object that allows for dynamic evaluation.
class Reader: public jsg::Object {
public:
  Reader() = default;

  void read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
    auto x = *BLAH;
    kj::byte* start = buf.begin();
    for (int i = 0; i < buf.size(); i++) {
      start[i] = x[offset + i];
    }
  }

  JSG_RESOURCE_TYPE(Reader) {
    JSG_METHOD(read);
  }
};


#define EW_PYODIDE_ISOLATE_TYPES api::pyodide::Reader

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getWorkerdExperimental() &&
      util::Autogate::isEnabled(util::AutogateKey::BUILTIN_WASM_MODULES)) {
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  }
  registry.template addBuiltinModule<Reader>("pyodide-internal:reader",
                                             workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

} // namespace workerd::api::pyodide
