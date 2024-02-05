#pragma once

#include <kj/common.h>
#include <pyodide/generated/pyodide_extra.capnp.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

// A function to read a segment of the tar file into a buffer
// Set up this way to avoid copying files that aren't accessed.
class PackagesTarReader : public jsg::Object {
public:
  PackagesTarReader() = default;

  int read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf);

  JSG_RESOURCE_TYPE(PackagesTarReader) {
    JSG_METHOD(read);
  }
};

// A function to read a segment of the tar file into a buffer
// Set up this way to avoid copying files that aren't accessed.
class PyodideMetadataReader : public jsg::Object {
private:
  kj::String mainModule;
  kj::Array<kj::String> names;
  kj::Array<kj::Array<kj::byte>> contents;
  kj::Array<kj::String> requirements;
  bool isWorkerdFlag;

public:
  PyodideMetadataReader(kj::String mainModule, kj::Array<kj::String> names,
                        kj::Array<kj::Array<kj::byte>> contents, kj::Array<kj::String> requirements,
                        bool isWorkerd)
      : mainModule(kj::mv(mainModule)), names(kj::mv(names)), contents(kj::mv(contents)),
        requirements(kj::mv(requirements)), isWorkerdFlag(isWorkerd) {}

  bool isWorkerd() {
    return this->isWorkerdFlag;
  }

  kj::String getMainModule() {
    return kj::str(this->mainModule);
  }

  kj::Array<jsg::JsRef<jsg::JsString>> getNames(jsg::Lock& js);

  kj::Array<jsg::JsRef<jsg::JsString>> getRequirements(jsg::Lock& js);

  kj::Array<int> getSizes(jsg::Lock& js);

  int read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf);

  JSG_RESOURCE_TYPE(PyodideMetadataReader) {
    JSG_METHOD(isWorkerd);
    JSG_METHOD(getMainModule);
    JSG_METHOD(getRequirements);
    JSG_METHOD(getNames);
    JSG_METHOD(getSizes);
    JSG_METHOD(read);
  }
};

using Worker = server::config::Worker;

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf);

#define EW_PYODIDE_ISOLATE_TYPES                                                                   \
  api::pyodide::PackagesTarReader, api::pyodide::PyodideMetadataReader

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getWorkerdExperimental()) {
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  }
  registry.template addBuiltinModule<PackagesTarReader>(
      "pyodide-internal:packages_tar_reader", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

} // namespace workerd::api::pyodide
