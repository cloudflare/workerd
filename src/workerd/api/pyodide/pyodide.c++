#include "pyodide.h"
#include <kj/string.h>
#include <workerd/util/string-buffer.h>
#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::api::pyodide {

// singleton that owns bundle

const kj::Maybe<jsg::Bundle::Reader> PyodideBundleManager::getPyodideBundle(
    kj::StringPtr version) const {
  KJ_IF_SOME(t, bundles.lockShared()->find(version)) {
    return t.bundle;
  }
  return kj::none;
}

void PyodideBundleManager::setPyodideBundleData(
    kj::String version, kj::Array<unsigned char> data) const {
  auto wordArray = kj::arrayPtr(
      reinterpret_cast<const capnp::word*>(data.begin()), data.size() / sizeof(capnp::word));
  auto messageReader = kj::heap<capnp::FlatArrayMessageReader>(wordArray).attach(kj::mv(data));
  auto bundle = messageReader->getRoot<jsg::Bundle>();
  bundles.lockExclusive()->insert(
      kj::mv(version), {.messageReader = kj::mv(messageReader), .bundle = bundle});
}

static int readToTarget(
    kj::ArrayPtr<const kj::byte> source, int offset, kj::ArrayPtr<kj::byte> buf) {
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
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->names[i]));
  }
  return builder.finish();
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getRequirements(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->requirements.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->requirements[i]));
  }
  return builder.finish();
}

kj::Array<int> PyodideMetadataReader::getSizes(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<int>(this->names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
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

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(
    Worker::Reader conf, const PythonConfig& pythonConfig) {
  auto modules = conf.getModules();
  auto mainModule = kj::str(modules.begin()->getName());
  int numFiles = 0;
  int numRequirements = 0;
  for (auto module: modules) {
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
  for (auto module: modules) {
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
  bool createSnapshot = pythonConfig.createSnapshot;
  bool createBaselineSnapshot = pythonConfig.createBaselineSnapshot;
  bool snapshotToDisk = createSnapshot || createBaselineSnapshot;
  // clang-format off
  return jsg::alloc<PyodideMetadataReader>(
    kj::mv(mainModule),
    names.finish(),
    contents.finish(),
    requirements.finish(),
    true      /* isWorkerd */,
    false     /* isTracing */,
    snapshotToDisk,
    createBaselineSnapshot,
    kj::none  /* memorySnapshot */
  );
  // clang-format on
}

const kj::Maybe<kj::Own<const kj::Directory>> DiskCache::NULL_CACHE_ROOT = kj::none;

jsg::Optional<kj::Array<kj::byte>> DiskCache::get(jsg::Lock& js, kj::String key) {
  KJ_IF_SOME(root, cacheRoot) {
    kj::Path path(key);
    auto file = root->tryOpenFile(path);

    KJ_IF_SOME(f, file) {
      return f->readAllBytes();
    } else {
      return kj::none;
    }
  } else {
    return kj::none;
  }
}

void DiskCache::put(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data) {
  KJ_IF_SOME(root, cacheRoot) {
    kj::Path path(key);
    auto file = root->tryOpenFile(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    KJ_IF_SOME(f, file) {
      f->writeAll(data);
    } else {
      KJ_LOG(ERROR, "DiskCache: Failed to open file", key);
    }
  } else {
    return;
  }
}

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules) {
  for (auto module: modules) {
    if (module.isPythonModule()) {
      return true;
    }
  }
  return false;
}

}  // namespace workerd::api::pyodide
