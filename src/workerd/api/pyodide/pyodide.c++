// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "pyodide.h"

#include "requirements.h"

#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/util/strings.h>

#include <openssl/sha.h>
#include <pyodide/generated/pyodide_extra.capnp.h>

#include <capnp/dynamic.h>
#include <capnp/schema.h>
#include <kj/array.h>
#include <kj/common.h>
#include <kj/compat/gzip.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/string.h>

#include <algorithm>  // for std::sort

namespace workerd::api::pyodide {

// singleton that owns bundle

const kj::Maybe<jsg::Bundle::Reader> PyodideBundleManager::getPyodideBundle(
    kj::StringPtr version) const {
  return bundles.lockShared()->find(version).map(
      [](const MessageBundlePair& t) { return t.bundle; });
}

void PyodideBundleManager::setPyodideBundleData(
    kj::String version, kj::Array<unsigned char> data) const {
  auto wordArray = kj::arrayPtr(
      reinterpret_cast<const capnp::word*>(data.begin()), data.size() / sizeof(capnp::word));
  // We're going to reuse this in the ModuleRegistry for every Python isolate, so set the traversal
  // limit to infinity or else eventually a new Python isolate will fail.
  auto messageReader = kj::heap<capnp::FlatArrayMessageReader>(
      wordArray, capnp::ReaderOptions{.traversalLimitInWords = kj::maxValue})
                           .attach(kj::mv(data));
  auto bundle = messageReader->getRoot<jsg::Bundle>();
  bundles.lockExclusive()->insert(
      kj::mv(version), {.messageReader = kj::mv(messageReader), .bundle = bundle});
}

const kj::Maybe<const kj::Array<unsigned char>&> PyodidePackageManager::getPyodidePackage(
    kj::StringPtr id) const {
  return packages.lockShared()->find(id);
}

void PyodidePackageManager::setPyodidePackageData(
    kj::String id, kj::Array<unsigned char> data) const {
  packages.lockExclusive()->insert(kj::mv(id), kj::mv(data));
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

int ReadOnlyBuffer::read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
  return readToTarget(source, offset, buf);
}

kj::Array<kj::StringPtr> PyodideMetadataReader::getNames(
    jsg::Lock& js, jsg::Optional<kj::String> maybeExtFilter) {
  auto builder = kj::Vector<kj::StringPtr>(state->moduleInfo.names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    KJ_IF_SOME(ext, maybeExtFilter) {
      if (!state->moduleInfo.names[i].endsWith(ext)) {
        continue;
      }
    }
    builder.add(state->moduleInfo.names[i]);
  }
  return builder.releaseAsArray();
}

void PyodideMetadataReader::setCpuLimitNearlyExceededCallback(
    jsg::Lock& js, kj::Array<kj::byte> wasm_memory, int sig_clock, int sig_flag) {
  // This callback has to be implemented in C++ because we don't hold the isolate lock when we call
  // it. It also has to be signal safe since we call it from the cpu time limiter.
  Worker::Isolate::from(js).setCpuLimitNearlyExceededCallback(
      [wasm_memory = kj::mv(wasm_memory), sig_clock, sig_flag]() mutable {
    // Set signal handling clock to fire on the next check.
    wasm_memory[sig_clock] = 0;
    // Set signal handling to on
    wasm_memory[sig_flag] = 1;
  });
}

kj::Array<kj::String> PythonModuleInfo::getPythonFileContents() {
  auto builder = kj::Vector<kj::String>(names.size());
  for (auto i: kj::zeroTo(names.size())) {
    if (names[i].endsWith(".py")) {
      builder.add(kj::str(contents[i].asChars()));
    }
  }
  return builder.releaseAsArray();
}

kj::HashSet<kj::String> PythonModuleInfo::getWorkerModuleSet() {
  auto result = kj::HashSet<kj::String>();
  const auto vendor = "python_modules/"_kj;
  const auto dotPy = ".py"_kj;
  const auto dotSo = ".so"_kj;
  for (auto& item: names) {
    kj::StringPtr name = item;
    if (name.startsWith(vendor)) {
      name = name.slice(vendor.size());
    }
    auto firstSlash = name.findFirst('/');
    KJ_IF_SOME(idx, firstSlash) {
      result.upsert(kj::str(name.slice(0, idx)), [](auto&&, auto&&) {});
      continue;
    }
    if (name.endsWith(dotPy)) {
      result.upsert(kj::str(name.slice(0, name.size() - dotPy.size())), [](auto&&, auto&&) {});
      continue;
    }
    if (name.endsWith(dotSo)) {
      result.upsert(kj::str(name.slice(0, name.size() - dotSo.size())), [](auto&&, auto&&) {});
      continue;
    }
  }
  return result;
}

kj::Array<int> PyodideMetadataReader::getSizes(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<int>(state->moduleInfo.names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(state->moduleInfo.contents[i].size());
  }
  return builder.finish();
}

int PyodideMetadataReader::read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf) {
  if (index >= state->moduleInfo.contents.size() || index < 0) {
    return 0;
  }
  auto& data = state->moduleInfo.contents[index];
  return readToTarget(data, offset, buf);
}

int PyodideMetadataReader::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (state->memorySnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(state->memorySnapshot), offset, buf);
}

kj::HashSet<kj::String> PyodideMetadataReader::getTransitiveRequirements() {
  auto packages = parseLockFile(state->packagesLock);
  auto depMap = getDepMapFromPackagesLock(*packages);

  return getPythonPackageNames(*packages, depMap, state->requirements, state->packagesVersion);
}

int ArtifactBundler::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (inner->existingSnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(inner->existingSnapshot), offset, buf);
}

const kj::Array<kj::StringPtr> snapshotImports = kj::arr("_pyodide"_kj,
    "_pyodide.docstring"_kj,
    "_pyodide._core_docs"_kj,
    "traceback"_kj,
    "collections.abc"_kj,
    // Asyncio is the really slow one here. In native Python on my machine, `import asyncio` takes ~50
    // ms.
    "asyncio"_kj,
    "inspect"_kj,
    "tarfile"_kj,
    "importlib",
    "importlib.metadata"_kj,
    "re"_kj,
    "shutil"_kj,
    "sysconfig"_kj,
    "importlib.machinery"_kj,
    "pathlib"_kj,
    "site"_kj,
    "tempfile"_kj,
    "typing"_kj,
    "zipfile"_kj);

kj::Array<kj::StringPtr> PyodideMetadataReader::getBaselineSnapshotImports() {
  return kj::heapArray(snapshotImports.begin(), snapshotImports.size());
}

jsg::JsObject PyodideMetadataReader::getCompatibilityFlags(jsg::Lock& js) {
  auto flags = FeatureFlags::get(js);
  auto obj = js.objNoProto();
  auto dynamic = capnp::toDynamic(flags);
  auto schema = dynamic.getSchema();

  for (auto field: schema.getFields()) {
    auto annotations = field.getProto().getAnnotations();

    // Note that disable flags are not exposed.
    for (auto annotation: annotations) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        obj.setReadOnly(
            js, annotation.getValue().getText(), js.boolean(dynamic.get(field).as<bool>()));
      }
    }
  }

  obj.seal(js);
  return obj;
}

PyodideMetadataReader::State::State(const State& other)
    : mainModule(kj::str(other.mainModule)),
      moduleInfo(other.moduleInfo.clone()),
      requirements(KJ_MAP(req, other.requirements) { return kj::str(req); }),
      pyodideVersion(kj::str(other.pyodideVersion)),
      packagesVersion(kj::str(other.packagesVersion)),
      packagesLock(kj::str(other.packagesLock)),
      isWorkerdFlag(other.isWorkerdFlag),
      isTracingFlag(other.isTracingFlag),
      snapshotToDisk(other.snapshotToDisk),
      createBaselineSnapshot(other.createBaselineSnapshot),
      memorySnapshot(other.memorySnapshot.map(
          [](auto& snapshot) { return kj::heapArray<kj::byte>(snapshot); })) {}

kj::Own<PyodideMetadataReader::State> PyodideMetadataReader::State::clone() {
  return kj::heap<PyodideMetadataReader::State>(*this);
}

void PyodideMetadataReader::State::verifyNoMainModuleInVendor() {
  // Verify that we don't have module named after the main module in the `python_modules` subdir.
  // mainModule includes the .py extension, so we need to extract the base name
  kj::ArrayPtr<const char> mainModuleBase = mainModule;
  if (mainModule.endsWith(".py")) {
    mainModuleBase = mainModuleBase.first(mainModuleBase.size() - 3);
  }

  for (auto& name: moduleInfo.names) {
    if (name.startsWith(kj::str("python_modules/", mainModule))) {
      JSG_FAIL_REQUIRE(
          Error, kj::str("Python module python_modules/", mainModule, " clashes with main module"));
    }
    if (name == kj::str("python_modules/", mainModuleBase, "/__init__.py")) {
      JSG_FAIL_REQUIRE(Error,
          kj::str("Python module python_modules/", mainModuleBase,
              "/__init__.py clashes with main module"));
    }
    if (name == kj::str("python_modules/", mainModuleBase, ".so")) {
      JSG_FAIL_REQUIRE(Error,
          kj::str("Python module python_modules/", mainModuleBase, ".so clashes with main module"));
    }
  }
}

kj::Maybe<kj::String> getPyodideLock(PythonSnapshotRelease::Reader pythonSnapshotRelease) {
  for (auto pkgLock: *PACKAGE_LOCKS) {
    if (pkgLock.getPackageDate() == pythonSnapshotRelease.getPackages()) {
      return kj::str(pkgLock.getLock());
    }
  }

  // From Pyodide 314 on, we don't use packages inside the lockfile.
  // All packages used by the worker should come from PyPI and be bundled inside the worker.
  // To avoid breaking existing workers, we return an empty lockfile if no packages are found.
  if (pythonSnapshotRelease.getPackages().size() == 0) {
    return kj::str("{\"packages\":{}}");
  }

  return kj::none;
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

void DiskCache::putSnapshot(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data) {
  KJ_IF_SOME(root, snapshotRoot) {
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

kj::String computePyodideBundleIntegrity(kj::ArrayPtr<const kj::byte> bytes) {
  kj::byte hash[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.begin(), bytes.size(), hash);
  return kj::str("sha256-", kj::encodeBase64(kj::arrayPtr(hash, SHA256_DIGEST_LENGTH)));
}

void verifyPyodideBundleIntegrity(
    kj::StringPtr version, kj::StringPtr expectedIntegrity, kj::ArrayPtr<const kj::byte> bytes) {
  // The "dev" bundle is built locally from the current tree and has no published checksum.
  if (version == "dev") {
    return;
  }
  // Every released bundle must have a published checksum; refuse to use one without it.
  KJ_REQUIRE(expectedIntegrity != nullptr && expectedIntegrity.size() > 0,
      "Pyodide bundle is missing an integrity checksum; refusing to use it.", version);
  auto actualIntegrity = computePyodideBundleIntegrity(bytes);
  KJ_REQUIRE(actualIntegrity == expectedIntegrity,
      "Pyodide bundle integrity check failed: the bundle does not match the expected checksum.",
      version, expectedIntegrity, actualIntegrity);
}

}  // namespace workerd::api::pyodide

namespace workerd {

struct PythonSnapshotParsedField {
  PythonSnapshotRelease::Reader pythonSnapshotRelease;
  capnp::StructSchema::Field field;
};

kj::Array<const PythonSnapshotParsedField> makePythonSnapshotFieldTable(
    capnp::StructSchema::FieldList fields) {
  kj::Vector<PythonSnapshotParsedField> table(fields.size());

  for (auto field: fields) {
    bool isPythonField = false;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == PYTHON_SNAPSHOT_RELEASE_ANNOTATION_ID) {
        isPythonField = true;
        break;
      }
    }
    if (!isPythonField) {
      continue;
    }

    auto name = field.getProto().getName();
    kj::Maybe<PythonSnapshotRelease::Reader> pythonSnapshotRelease;
    for (auto release: *RELEASES) {
      if (release.getFlagName() == name) {
        pythonSnapshotRelease = release;
        break;
      }
    }
    table.add(PythonSnapshotParsedField{
      .pythonSnapshotRelease = KJ_REQUIRE_NONNULL(pythonSnapshotRelease),
      .field = field,
    });
  }

  return table.releaseAsArray();
}

kj::Maybe<PythonSnapshotRelease::Reader> getPythonSnapshotRelease(
    CompatibilityFlags::Reader featureFlags) {
  uint latestFieldOrdinal = 0;
  kj::Maybe<PythonSnapshotRelease::Reader> result;

  static const auto fieldTable =
      makePythonSnapshotFieldTable(capnp::Schema::from<CompatibilityFlags>().getFields());

  for (auto field: fieldTable) {
    bool isEnabled = capnp::toDynamic(featureFlags).get(field.field).as<bool>();
    if (!isEnabled) {
      continue;
    }

    // We pick the flag with the highest ordinal value that is enabled and has a
    // pythonSnapshotRelease annotation.
    //
    // The fieldTable is probably ordered by the ordinal anyway, but doesn't hurt to be explicit
    // here.
    if (latestFieldOrdinal < field.field.getIndex()) {
      latestFieldOrdinal = field.field.getIndex();
      result = field.pythonSnapshotRelease;
    }
  }

  return result;
}

kj::String getPythonBundleName(PythonSnapshotRelease::Reader pyodideRelease) {
  if (pyodideRelease.getPyodide() == "dev") {
    return kj::str("dev");
  }
  return kj::str(pyodideRelease.getPyodide(), "_", pyodideRelease.getPyodideRevision(), "_",
      pyodideRelease.getBackport());
}

namespace api::pyodide {

// Returns a string containing the contents of the hashset, delimited by ", "
kj::String hashsetToString(const kj::HashSet<kj::String>& set) {
  if (set.size() == 0) {
    return kj::String();
  }

  kj::Vector<kj::StringPtr> elems;
  for (const auto& e: set) {
    elems.add(e);
  }

  // Sort the elements for consistent output
  auto array = elems.releaseAsArray();
  std::sort(array.begin(), array.end());

  return kj::str(kj::delimited(array, ", "_kjc));
}

kj::Array<kj::String> getPythonPackageFiles(kj::StringPtr lockFileContents,
    kj::ArrayPtr<kj::String> requirements,
    kj::StringPtr packagesVersion) {
  auto packages = parseLockFile(lockFileContents);
  auto depMap = getDepMapFromPackagesLock(*packages);

  auto allRequirements = getPythonPackageNames(*packages, depMap, requirements, packagesVersion);

  // Add the file names of all the requirements to our result array.
  kj::Vector<kj::String> res;
  for (const auto& ent: *packages) {
    auto name = ent.getName();
    auto obj = ent.getValue().getObject();
    auto fileName = kj::str(getField(obj, "file_name").getString());

    auto maybeRow = allRequirements.find(name);
    KJ_IF_SOME(row, maybeRow) {
      allRequirements.erase(row);
      res.add(kj::mv(fileName));
    } else if (packagesVersion == "20240829.4") {
      auto packageType = getField(obj, "package_type").getString();
      if (packageType == "cpython_module") {
        res.add(kj::mv(fileName));
      }
    }
  }

  if (allRequirements.size() != 0) {
    JSG_FAIL_REQUIRE(Error,
        "Requested Python package(s) that are not supported: ", hashsetToString(allRequirements));
  }

  return res.releaseAsArray();
}

void WorkerFatalReporter::reportFatal(jsg::Lock& js, kj::String error) {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    kj::runCatchingExceptions([&]() { ioContext.getMetrics().setWorkerFatal(); });
  }
}

void WorkerFatalReporter::reportPythonWorkersInternalError(jsg::Lock& js) {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    kj::runCatchingExceptions([&]() { ioContext.getMetrics().setPythonWorkersInternalError(); });
  }
}

}  // namespace api::pyodide

}  // namespace workerd
