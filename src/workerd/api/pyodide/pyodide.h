#pragma once

#include <kj/common.h>
#include <pyodide/generated/pyodide_packages.capnp.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>

namespace workerd::api::pyodide {

// A function to read a segment of the tar file into a buffer
// Set up this way to avoid copying files that aren't accessed.
class PackagesTarReader : public jsg::Object {
public:
  PackagesTarReader() = default;

  int read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
    int tarSize = PYODIDE_PACKAGES_TAR->size();
    if (offset >= tarSize || offset < 0) {
      return 0;
    }
    int toCopy = buf.size();
    if (tarSize - offset < toCopy) {
      toCopy = tarSize - offset;
    }
    memcpy(buf.begin(), &((*PYODIDE_PACKAGES_TAR)[0]) + offset, toCopy);
    return toCopy;
  }

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
                        bool isWorkerd) {
    this->mainModule = kj::mv(mainModule);
    this->names = kj::mv(names);
    this->contents = kj::mv(contents);
    this->requirements = kj::mv(requirements);
    this->isWorkerdFlag = isWorkerd;
  }

  bool isWorkerd() {
    return this->isWorkerdFlag;
  }

  kj::String getMainModule() {
    return kj::str(this->mainModule);
  }

  kj::Array<jsg::JsRef<jsg::JsString>> getNames(jsg::Lock& js) {
    auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->names.size());
    for (auto i : kj::zeroTo(builder.capacity())) {
      builder.add(js, js.str(this->names[i]));
    }
    return builder.finish();
  }

  kj::Array<jsg::JsRef<jsg::JsString>> getRequirements(jsg::Lock& js) {
    auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->requirements.size());
    for (auto i : kj::zeroTo(builder.capacity())) {
      builder.add(js, js.str(this->requirements[i]));
    }
    return builder.finish();
  }

  kj::Array<int> getSizes(jsg::Lock& js) {
    auto builder = kj::heapArrayBuilder<int>(this->names.size());
    for (auto i : kj::zeroTo(builder.capacity())) {
      builder.add(this->contents[i].size());
    }
    return builder.finish();
  }

  int read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf) {
    if (index >= contents.size() || index < 0) {
      return 0;
    }
    auto& data = contents[index];
    int dataSize = data.size();
    if (offset >= dataSize || offset < 0) {
      return 0;
    }
    int toCopy = buf.size();
    if (dataSize - offset < toCopy) {
      toCopy = dataSize - offset;
    }
    memcpy(buf.begin(), &data[0] + offset, toCopy);
    return toCopy;
  }

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

inline jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf) {
  auto modules = conf.getModules();
  auto mainModule = kj::str(modules.begin()->getName());
  int numFiles = 0;
  int numRequirements = 0;
  for (auto module : modules) {
    switch (module.which()) {
    case Worker::Module::TEXT:
    case Worker::Module::DATA:
    case Worker::Module::JSON:
    case Worker::Module::PYTHON_MODULE:
      numFiles++;
      break;
    case Worker::Module::PYTHON_REQUIREMENT:
      numRequirements++;
      break;
    default:
      break;
    }
  }

  auto names = kj::heapArrayBuilder<kj::String>(numFiles);
  auto contents = kj::heapArrayBuilder<kj::Array<kj::byte>>(numFiles);
  auto requirements = kj::heapArrayBuilder<kj::String>(numRequirements);
  for (auto module : modules) {
    bool pymodule = false;
    switch (module.which()) {
    case Worker::Module::TEXT:
      contents.add(kj::heapArray(module.getText().asBytes()));
      break;
    case Worker::Module::DATA:
      contents.add(kj::heapArray(module.getData().asBytes()));
      break;
    case Worker::Module::JSON:
      contents.add(kj::heapArray(module.getJson().asBytes()));
      break;
    case Worker::Module::PYTHON_MODULE:
      pymodule = true;
      contents.add(kj::heapArray(module.getPythonModule().asBytes()));
      break;
    case Worker::Module::PYTHON_REQUIREMENT:
      requirements.add(kj::str(module.getName()));
      continue;
    default:
      continue;
    }
    auto name = module.getName();
    if (pymodule && !name.endsWith(".py")) {
      names.add(kj::str(name, ".py"));
    } else {
      names.add(kj::str(name));
    }
  }
  return jsg::alloc<PyodideMetadataReader>(kj::mv(mainModule), names.finish(), contents.finish(),
                                           requirements.finish(), true);
}

#define EW_PYODIDE_ISOLATE_TYPES api::pyodide::PackagesTarReader, api::pyodide::PyodideMetadataReader

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getWorkerdExperimental() &&
      util::Autogate::isEnabled(util::AutogateKey::BUILTIN_WASM_MODULES)) {
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  }
  registry.template addBuiltinModule<PackagesTarReader>(
      "pyodide-internal:packages_tar_reader", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

} // namespace workerd::api::pyodide
