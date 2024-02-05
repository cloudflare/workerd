#include "pyodide.h"

namespace workerd::api::pyodide {

int PackagesTarReader::read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
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

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getNames(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->names.size());
  for (auto i : kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->names[i]));
  }
  return builder.finish();
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getRequirements(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->requirements.size());
  for (auto i : kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->requirements[i]));
  }
  return builder.finish();
}

kj::Array<int> PyodideMetadataReader::getSizes(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<int>(this->names.size());
  for (auto i : kj::zeroTo(builder.capacity())) {
    builder.add(this->contents[i].size());
  }
  return builder.finish();
}

int PyodideMetadataReader::read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf) {
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

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf) {
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

} // namespace workerd::api::pyodide
