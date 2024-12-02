// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "workerd/util/wait-list.h"

#include <workerd/api/pyodide/setup-emscripten.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>

#include <pyodide/generated/pyodide_extra.capnp.h>

#include <capnp/serialize.h>
#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/filesystem.h>

namespace workerd::api::pyodide {

class PyodideBundleManager {
 public:
  void setPyodideBundleData(kj::String version, kj::Array<unsigned char> data) const;
  const kj::Maybe<jsg::Bundle::Reader> getPyodideBundle(kj::StringPtr version) const;

 private:
  struct MessageBundlePair {
    kj::Own<capnp::FlatArrayMessageReader> messageReader;
    jsg::Bundle::Reader bundle;
  };
  const kj::MutexGuarded<kj::HashMap<kj::String, MessageBundlePair>> bundles;
};

class PyodidePackageManager {
 public:
  void setPyodidePackageData(kj::String id, kj::Array<unsigned char> data) const;
  const kj::Maybe<const kj::Array<unsigned char>&> getPyodidePackage(kj::StringPtr id) const;

 private:
  const kj::MutexGuarded<kj::HashMap<kj::String, kj::Array<unsigned char>>> packages;
};

struct PythonConfig {
  kj::Maybe<kj::Own<const kj::Directory>> packageDiskCacheRoot;
  kj::Maybe<kj::Own<const kj::Directory>> pyodideDiskCacheRoot;
  const PyodideBundleManager pyodideBundleManager;
  bool createSnapshot;
  bool createBaselineSnapshot;
};

// A function to read a segment of the tar file into a buffer
// Set up this way to avoid copying files that aren't accessed.
class ReadOnlyBuffer: public jsg::Object {
  kj::ArrayPtr<const kj::byte> source;

 public:
  ReadOnlyBuffer(kj::ArrayPtr<const kj::byte> src): source(src) {};

  int read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf);

  JSG_RESOURCE_TYPE(ReadOnlyBuffer) {
    JSG_METHOD(read);
  }
};

// A class wrapping the information stored in a WorkerBundle, in particular the Python source files
// and metadata about the worker.
//
// This is done this way to avoid copying files as much as possible. We set up a Metadata File
// System which reads the contents as they are needed.
class PyodideMetadataReader: public jsg::Object {
 private:
  kj::String mainModule;
  kj::Array<kj::String> names;
  kj::Array<kj::Array<kj::byte>> contents;
  kj::Array<kj::String> requirements;
  kj::String packagesVersion;
  kj::String packagesLock;
  bool isWorkerdFlag;
  bool isTracingFlag;
  bool snapshotToDisk;
  bool createBaselineSnapshot;
  bool usePackagesInArtifactBundler;
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot;

 public:
  PyodideMetadataReader(kj::String mainModule,
      kj::Array<kj::String> names,
      kj::Array<kj::Array<kj::byte>> contents,
      kj::Array<kj::String> requirements,
      kj::String packagesVersion,
      kj::String packagesLock,
      bool isWorkerd,
      bool isTracing,
      bool snapshotToDisk,
      bool createBaselineSnapshot,
      bool usePackagesInArtifactBundler,
      kj::Maybe<kj::Array<kj::byte>> memorySnapshot)
      : mainModule(kj::mv(mainModule)),
        names(kj::mv(names)),
        contents(kj::mv(contents)),
        requirements(kj::mv(requirements)),
        packagesVersion(kj::mv(packagesVersion)),
        packagesLock(kj::mv(packagesLock)),
        isWorkerdFlag(isWorkerd),
        isTracingFlag(isTracing),
        snapshotToDisk(snapshotToDisk),
        createBaselineSnapshot(createBaselineSnapshot),
        usePackagesInArtifactBundler(usePackagesInArtifactBundler),
        memorySnapshot(kj::mv(memorySnapshot)) {}

  bool isWorkerd() {
    return this->isWorkerdFlag;
  }

  bool isTracing() {
    return this->isTracingFlag;
  }

  bool shouldSnapshotToDisk() {
    return snapshotToDisk;
  }

  bool isCreatingBaselineSnapshot() {
    return createBaselineSnapshot;
  }

  kj::String getMainModule() {
    return kj::str(this->mainModule);
  }

  kj::Array<jsg::JsRef<jsg::JsString>> getNames(jsg::Lock& js);

  // Returns files inside the WorkerBundle that end with the specified file extension.
  // Usually called to get all the Python source files with a `py` extension.
  kj::Array<jsg::JsRef<jsg::JsString>> getWorkerFiles(jsg::Lock& js, kj::String ext);

  kj::Array<jsg::JsRef<jsg::JsString>> getRequirements(jsg::Lock& js);

  kj::Array<int> getSizes(jsg::Lock& js);

  int read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf);

  bool hasMemorySnapshot() {
    return memorySnapshot != kj::none;
  }
  int getMemorySnapshotSize() {
    if (memorySnapshot == kj::none) {
      return 0;
    }
    return KJ_REQUIRE_NONNULL(memorySnapshot).size();
  }

  void disposeMemorySnapshot() {
    memorySnapshot = kj::none;
  }
  int readMemorySnapshot(int offset, kj::Array<kj::byte> buf);

  bool shouldUsePackagesInArtifactBundler() {
    return usePackagesInArtifactBundler;
  }

  kj::String getPackagesVersion() {
    return kj::str(packagesVersion);
  }

  kj::String getPackagesLock() {
    return kj::str(packagesLock);
  }

  JSG_RESOURCE_TYPE(PyodideMetadataReader) {
    JSG_METHOD(isWorkerd);
    JSG_METHOD(isTracing);
    JSG_METHOD(getMainModule);
    JSG_METHOD(getRequirements);
    JSG_METHOD(getNames);
    JSG_METHOD(getWorkerFiles);
    JSG_METHOD(getSizes);
    JSG_METHOD(read);
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(shouldSnapshotToDisk);
    JSG_METHOD(shouldUsePackagesInArtifactBundler);
    JSG_METHOD(getPackagesVersion);
    JSG_METHOD(getPackagesLock);
    JSG_METHOD(isCreatingBaselineSnapshot);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("mainModule", mainModule);
    for (const auto& name: names) {
      tracker.trackField("name", name);
    }
    for (const auto& content: contents) {
      tracker.trackField("content", content);
    }
    for (const auto& requirement: requirements) {
      tracker.trackField("requirement", requirement);
    }
  }
};

struct MemorySnapshotResult {
  kj::Array<kj::byte> snapshot;
  kj::Array<kj::String> importedModulesList;
  JSG_STRUCT(snapshot, importedModulesList);
};

// A loaded bundle of artifacts for a particular script id. It can also contain V8 version and
// CPU architecture-specific artifacts. The logic for loading these is in getArtifacts.
class ArtifactBundler: public jsg::Object {
 public:
  kj::Maybe<const PyodidePackageManager&> packageManager;
  // ^ lifetime should be contained by lifetime of ArtifactBundler since there is normally one worker set for the whole process. see worker-set.h
  // In other words:
  // WorkerSet lifetime = PackageManager lifetime and Worker lifetime = ArtifactBundler lifetime and WorkerSet owns and will outlive Worker, so PackageManager outlives ArtifactBundler
  kj::Maybe<MemorySnapshotResult> storedSnapshot;

  ArtifactBundler(kj::Maybe<const PyodidePackageManager&> packageManager,
      kj::Maybe<kj::Array<const kj::byte>> existingSnapshot,
      bool isValidating = false)
      : packageManager(packageManager),
        storedSnapshot(kj::none),
        existingSnapshot(kj::mv(existingSnapshot)),
        isValidating(isValidating) {};

  void storeMemorySnapshot(jsg::Lock& js, MemorySnapshotResult snapshot) {
    KJ_REQUIRE(isValidating);
    storedSnapshot = kj::mv(snapshot);
  }

  bool hasMemorySnapshot() {
    return existingSnapshot != kj::none;
  }

  int getMemorySnapshotSize() {
    if (existingSnapshot == kj::none) {
      return 0;
    }
    return KJ_REQUIRE_NONNULL(existingSnapshot).size();
  }

  int readMemorySnapshot(int offset, kj::Array<kj::byte> buf);
  void disposeMemorySnapshot() {
    existingSnapshot = kj::none;
  }

  // Determines whether this ArtifactBundler was created inside the validator.
  bool isEwValidating() {
    return isValidating;
  }

  static jsg::Ref<ArtifactBundler> makeDisabledBundler() {
    return jsg::alloc<ArtifactBundler>(kj::none, kj::none);
  }

  // Creates an ArtifactBundler that only grants access to packages, and not a memory snapshot.
  static jsg::Ref<ArtifactBundler> makePackagesOnlyBundler(
      kj::Maybe<const PyodidePackageManager&> manager) {
    return jsg::alloc<ArtifactBundler>(manager, kj::none);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    if (existingSnapshot == kj::none) {
      return;
    }
    tracker.trackFieldWithSize("snapshot", KJ_REQUIRE_NONNULL(existingSnapshot).size());
  }

  bool isEnabled() {
    return false;  // TODO(later): Remove this function once we regenerate the bundle.
  }

  kj::Maybe<jsg::Ref<ReadOnlyBuffer>> getPackage(kj::String path) {
    KJ_IF_SOME(pacman, packageManager) {
      KJ_IF_SOME(ptr, pacman.getPyodidePackage(path)) {
        return jsg::alloc<ReadOnlyBuffer>(ptr);
      }
    }

    return kj::none;
  }

  // Takes in a list of Python files (their contents). Parses these files to find the import
  // statements, then returns a list of modules imported via those statements.
  //
  // For example:
  // import a, b, c
  // from z import x
  // import t.y.u
  // from . import k
  //
  // -> ["a", "b", "c", "z", "t.y.u"]
  //
  // Package relative imports are ignored.
  static kj::Array<kj::String> parsePythonScriptImports(kj::Array<kj::String> files);
  // Takes in a list of imported modules and filters them in such a way to avoid local imports and
  // redundant imports in the package snapshot list.
  static kj::Array<kj::String> filterPythonScriptImports(
      kj::HashSet<kj::String> locals, kj::Array<kj::String> imports);
  static kj::Array<kj::String> filterPythonScriptImportsJs(
      kj::Array<kj::String> locals, kj::Array<kj::String> imports);
  static kj::Array<kj::StringPtr> getSnapshotImports();

  JSG_RESOURCE_TYPE(ArtifactBundler) {
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(isEwValidating);
    JSG_METHOD(storeMemorySnapshot);
    JSG_METHOD(isEnabled);
    JSG_METHOD(getPackage);
    JSG_STATIC_METHOD(parsePythonScriptImports);
    JSG_STATIC_METHOD(filterPythonScriptImportsJs);
    JSG_STATIC_METHOD(getSnapshotImports);
  }

 private:
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<const kj::byte>> existingSnapshot;
  bool isValidating;
};

class DisabledInternalJaeger: public jsg::Object {
 public:
  static jsg::Ref<DisabledInternalJaeger> create() {
    return jsg::alloc<DisabledInternalJaeger>();
  }
  JSG_RESOURCE_TYPE(DisabledInternalJaeger) {}
};

// This cache is used by Pyodide to store wheels fetched over the internet across workerd restarts in local dev only
class DiskCache: public jsg::Object {
 private:
  static const kj::Maybe<kj::Own<const kj::Directory>> NULL_CACHE_ROOT;  // always set to kj::none

  const kj::Maybe<kj::Own<const kj::Directory>>& cacheRoot;

 public:
  DiskCache(): cacheRoot(NULL_CACHE_ROOT) {};  // Disabled disk cache
  DiskCache(const kj::Maybe<kj::Own<const kj::Directory>>& cacheRoot): cacheRoot(cacheRoot) {};

  static jsg::Ref<DiskCache> makeDisabled() {
    return jsg::alloc<DiskCache>();
  }

  jsg::Optional<kj::Array<kj::byte>> get(jsg::Lock& js, kj::String key);
  void put(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data);

  JSG_RESOURCE_TYPE(DiskCache) {
    JSG_METHOD(get);
    JSG_METHOD(put);
  }
};

// A limiter which will throw if the startup is found to exceed limits. The script will still be
// able to run for longer than the limit, but an error will be thrown as soon as the startup
// finishes. This way we can enforce a Python-specific startup limit.
//
// TODO(later): stop execution as soon limit is reached, instead of doing so after the fact.
class SimplePythonLimiter: public jsg::Object {
 private:
  int startupLimitMs;
  kj::Maybe<kj::Function<kj::TimePoint()>> getTimeCb;

  kj::Maybe<kj::TimePoint> startTime;

 public:
  SimplePythonLimiter(): startupLimitMs(0), getTimeCb(kj::none) {}

  SimplePythonLimiter(int startupLimitMs, kj::Function<kj::TimePoint()> getTimeCb)
      : startupLimitMs(startupLimitMs),
        getTimeCb(kj::mv(getTimeCb)) {}

  static jsg::Ref<SimplePythonLimiter> makeDisabled() {
    return jsg::alloc<SimplePythonLimiter>();
  }

  void beginStartup() {
    KJ_IF_SOME(cb, getTimeCb) {
      JSG_REQUIRE(startTime == kj::none, TypeError, "Cannot call `beginStartup` multiple times.");
      startTime = cb();
    }
  }

  void finishStartup() {
    KJ_IF_SOME(cb, getTimeCb) {
      JSG_REQUIRE(startTime != kj::none, TypeError, "Need to call `beginStartup` first.");
      auto endTime = cb();
      kj::Duration diff = endTime - KJ_ASSERT_NONNULL(startTime);
      auto diffMs = diff / kj::MILLISECONDS;

      JSG_REQUIRE(diffMs <= startupLimitMs, TypeError, "Python Worker startup exceeded CPU limit");
    }
  }

  JSG_RESOURCE_TYPE(SimplePythonLimiter) {
    JSG_METHOD(beginStartup);
    JSG_METHOD(finishStartup);
  }
};

class SetupEmscripten: public jsg::Object {
 public:
  SetupEmscripten(EmscriptenRuntime emscriptenRuntime)
      : emscriptenRuntime(kj::mv(emscriptenRuntime)) {};

  jsg::JsValue getModule(jsg::Lock& js);

  JSG_RESOURCE_TYPE(SetupEmscripten) {
    JSG_METHOD(getModule);
  }

 private:
  EmscriptenRuntime emscriptenRuntime;
  void visitForGc(jsg::GcVisitor& visitor);
};

using Worker = server::config::Worker;

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(
    Worker::Reader conf, const PythonConfig& pythonConfig);

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules);

#define EW_PYODIDE_ISOLATE_TYPES                                                                   \
  api::pyodide::ReadOnlyBuffer, api::pyodide::PyodideMetadataReader,                               \
      api::pyodide::ArtifactBundler, api::pyodide::DiskCache,                                      \
      api::pyodide::DisabledInternalJaeger, api::pyodide::SimplePythonLimiter,                     \
      api::pyodide::MemorySnapshotResult, api::pyodide::SetupEmscripten

}  // namespace workerd::api::pyodide
