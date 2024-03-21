#include "pyodide.h"
#include "kj/array.h"
#include "kj/common.h"
#include "kj/debug.h"

namespace workerd::api::pyodide {

static int readToTarget(kj::ArrayPtr<const kj::byte> source, int offset, kj::ArrayPtr<kj::byte> buf) {
  int size = source.size();
  if (offset >= size || offset < 0) {
    return 0;
  }
  int toCopy = buf.size();
  if (size - offset < toCopy) {
    toCopy = size - offset;
  }
  memcpy(buf.begin(), source.begin() + offset, toCopy);
  return toCopy;
}

int PackagesTarReader::read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
  return readToTarget(PYODIDE_PACKAGES_TAR.get(), offset, buf);
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
  return readToTarget(data, offset, buf);
}

int PyodideMetadataReader::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (memorySnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(memorySnapshot), offset, buf);
}

int ArtifactBundler::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (existingSnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(existingSnapshot), offset, buf);
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
      KJ_REQUIRE(module.getName().endsWith(".py"));
      contents.add(kj::heapArray(module.getPythonModule().asBytes()));
      break;
    case Worker::Module::PYTHON_REQUIREMENT:
      requirements.add(kj::str(module.getName()));
      continue;
    default:
      continue;
    }
    names.add(kj::str(module.getName()));
  }
  return jsg::alloc<PyodideMetadataReader>(kj::mv(mainModule), names.finish(), contents.finish(),
                                           requirements.finish(), true /* isWorkerd */,
                                           false /* isTracing */, kj::none /* memorySnapshot */);
}

} // namespace workerd::api::pyodide
