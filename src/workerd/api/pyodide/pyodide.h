// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/util/strong-bool.h>

#include <pyodide/generated/pyodide_extra.capnp.h>
#include <pyodide/pyodide_static.capnp.h>
#include <pyodide/python_packages.capnp.h>

#include <capnp/serialize.h>
#include <kj/array.h>
#include <kj/common.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/function.h>
#include <kj/string.h>
#include <kj/table.h>
#include <kj/timer.h>

namespace workerd::api::pyodide {

WD_STRONG_BOOL(CreateBaselineSnapshot);
WD_STRONG_BOOL(IsTracing);
WD_STRONG_BOOL(IsValidating);
WD_STRONG_BOOL(IsWorkerd);
WD_STRONG_BOOL(SnapshotToDisk);

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

struct PythonConfig {
  kj::Maybe<kj::Own<const kj::Directory>> packageDiskCacheRoot;
  kj::Maybe<kj::Own<const kj::Directory>> pyodideDiskCacheRoot;
  kj::Maybe<kj::Own<const kj::Directory>> snapshotDirectory;
  const PyodideBundleManager pyodideBundleManager;
  bool createSnapshot;
  bool createBaselineSnapshot;
  kj::Maybe<kj::String> loadSnapshotFromDisk;
};

// A function to read a segment of a buffer (e.g. an embedded package file) into a target buffer.
// Set up this way to avoid copying files that aren't accessed.
class ReadOnlyBuffer: public jsg::Object {
  kj::ArrayPtr<const kj::byte> source;

 public:
  ReadOnlyBuffer(kj::ArrayPtr<const kj::byte> src): source(src) {};

  uint32_t read(jsg::Lock& js, uint64_t offset, kj::Array<kj::byte> buf);

  JSG_RESOURCE_TYPE(ReadOnlyBuffer) {
    JSG_METHOD(read);
  }
};

// Metadata for a single embedded Python package file, returned to the runtime so it can build the
// site-packages / dynlib filesystem (see src/pyodide/internal/loadPackage.ts). The string fields
// point directly into the (process-lifetime) bundle message to avoid copying; they are only copied
// when JSG marshals them into V8 strings.
struct PythonPackageFileMetadata {
  // Mount root ("site"/"stdlib" -> site-packages, "dynlib" -> /usr/lib).
  kj::StringPtr installDir;
  // Path within `installDir`, e.g. "ssl/__init__.py".
  kj::StringPtr path;
  // Size of the file contents in bytes.
  int size;
  // Reader for the (already-decompressed) bytes of this file.
  jsg::Ref<ReadOnlyBuffer> reader;
  JSG_STRUCT(installDir, path, size, reader);
};

// Exposes the Python stdlib package files that are extracted and embedded directly in the Pyodide
// bundle as a PythonPackages capnp message (see python_packages.capnp / pack_python_packages.py).
// The runtime reads `getFiles()` to learn the file layout; each returned entry carries a `reader`
// for the (already-decompressed) bytes of that file. This is a single bulk call (rather than a
// per-file accessor) to avoid a JS<->C++ round-trip per file.
class EmbeddedPackagesReader: public jsg::Object {
 public:
  EmbeddedPackagesReader(kj::Maybe<kj::Own<capnp::FlatArrayMessageReader>> messageReader)
      : messageReader(kj::mv(messageReader)) {}

  // Builds a reader from a Pyodide bundle, locating the embedded `python_packages` data module. If
  // the bundle has no embedded packages, the returned reader exposes an empty file list.
  static jsg::Ref<EmbeddedPackagesReader> fromBundle(jsg::Lock& js, jsg::Bundle::Reader bundle);

  kj::Array<PythonPackageFileMetadata> getFiles(jsg::Lock& js);

  JSG_RESOURCE_TYPE(EmbeddedPackagesReader) {
    JSG_METHOD(getFiles);
  }

 private:
  // Owns the message backing `files`. `kj::none` when the bundle has no embedded packages.
  kj::Maybe<kj::Own<capnp::FlatArrayMessageReader>> messageReader;

  kj::Maybe<PythonPackages::Reader> files() {
    return messageReader.map([](kj::Own<capnp::FlatArrayMessageReader>& reader) {
      return reader->getRoot<PythonPackages>();
    });
  }
};

class PythonModuleInfo {
 public:
  PythonModuleInfo(kj::Array<kj::String> names, kj::Array<kj::Array<kj::byte>> contents)
      : names(kj::mv(names)),
        contents(kj::mv(contents)) {
    KJ_REQUIRE(this->names.size() == this->contents.size());
  }
  kj::Array<kj::String> names;
  kj::Array<kj::Array<kj::byte>> contents;

  PythonModuleInfo clone() const {
    auto clonedContents =
        KJ_MAP(content, this->contents) { return kj::heapArray<kj::byte>(content); };
    auto clonedNames = KJ_MAP(name, this->names) { return kj::str(name); };
    return PythonModuleInfo(kj::mv(clonedNames), kj::mv(clonedContents));
  }

  kj::HashSet<kj::String> getWorkerModuleSet();
  kj::Array<kj::String> getPythonFileContents();
};

// A class wrapping the information stored in a WorkerBundle, in particular the Python source files
// and metadata about the worker.
//
// This is done this way to avoid copying files as much as possible. We set up a Metadata File
// System which reads the contents as they are needed.
class PyodideMetadataReader: public jsg::Object {
 public:
  //
  struct State {
    kj::String mainModule;
    PythonModuleInfo moduleInfo;
    kj::String pyodideVersion;
    kj::String packagesVersion;
    kj::String packagesLock;
    bool isWorkerdFlag;
    bool isTracingFlag;
    bool snapshotToDisk;
    bool createBaselineSnapshot;
    kj::Maybe<kj::Array<kj::byte>> memorySnapshot;

    State(kj::String mainModule,
        kj::Array<kj::String> names,
        kj::Array<kj::Array<kj::byte>> contents,
        kj::String pyodideVersion,
        kj::String packagesVersion,
        kj::String packagesLock,
        IsWorkerd isWorkerd,
        IsTracing isTracing,
        SnapshotToDisk snapshotToDisk,
        CreateBaselineSnapshot createBaselineSnapshot,
        kj::Maybe<kj::Array<kj::byte>> memorySnapshot)
        : mainModule(kj::mv(mainModule)),
          moduleInfo(kj::mv(names), kj::mv(contents)),
          pyodideVersion(kj::mv(pyodideVersion)),
          packagesVersion(kj::mv(packagesVersion)),
          packagesLock(kj::mv(packagesLock)),
          isWorkerdFlag(isWorkerd),
          isTracingFlag(isTracing),
          snapshotToDisk(snapshotToDisk),
          createBaselineSnapshot(createBaselineSnapshot),
          memorySnapshot(kj::mv(memorySnapshot)) {
      verifyNoMainModuleInVendor();
    }

    State(const State& other);

    void verifyNoMainModuleInVendor();

    kj::Own<State> clone();
  };

  PyodideMetadataReader(kj::Own<State> state): state(kj::mv(state)) {}

  bool isWorkerd() {
    return state->isWorkerdFlag;
  }

  bool isTracing() {
    return state->isTracingFlag;
  }

  bool shouldSnapshotToDisk() {
    return state->snapshotToDisk;
  }

  bool isCreatingBaselineSnapshot() {
    return state->createBaselineSnapshot;
  }

  kj::StringPtr getMainModule() {
    return state->mainModule;
  }

  // Returns the filenames of the files inside of the WorkerBundle that end with the specified
  // file extension.
  // TODO: Remove this.
  kj::Array<kj::StringPtr> getNames(jsg::Lock& js, jsg::Optional<kj::String> maybeExtFilter);
  kj::Array<uint32_t> getSizes(jsg::Lock& js);

  uint32_t read(jsg::Lock& js, uint64_t index, uint64_t offset, kj::Array<kj::byte> buf);

  bool hasMemorySnapshot() {
    return state->memorySnapshot != kj::none;
  }
  uint32_t getMemorySnapshotSize() {
    if (state->memorySnapshot == kj::none) {
      return 0;
    }
    auto size = KJ_REQUIRE_NONNULL(state->memorySnapshot).size();
    KJ_REQUIRE(size <= static_cast<uint32_t>(kj::maxValue), "memory snapshot too large");
    return size;
  }

  void disposeMemorySnapshot() {
    state->memorySnapshot = kj::none;
  }
  uint32_t readMemorySnapshot(uint64_t offset, kj::Array<kj::byte> buf);

  kj::StringPtr getPyodideVersion() {
    return state->pyodideVersion;
  }

  kj::StringPtr getPackagesVersion() {
    return state->packagesVersion;
  }

  kj::StringPtr getPackagesLock() {
    return state->packagesLock;
  }

  static kj::Array<kj::StringPtr> getBaselineSnapshotImports();

  // We call this during Python setup with the wasm memory and the addresses of the signal clock and
  // the flag to indicate whether signal handling is on or off. It sets up the isolate
  // CpuLimitNearlyExceeded callback to trigger a signal in Python.
  void setCpuLimitNearlyExceededCallback(
      jsg::Lock& js, kj::Array<kj::byte> wasm_memory, int sig_clock, int sig_flag);

  // Similar to Cloudflare::::getCompatibilityFlags in global-scope.c++, but the key difference is
  // that it returns experimental flags even if `experimental` is not enabled. This avoids a gotcha
  // where an experimental compat flag is enabled in our C++ code, but not in our JS code.
  //
  // This is only for use by our Python runtime.
  jsg::JsObject getCompatibilityFlags(jsg::Lock& js);

  JSG_RESOURCE_TYPE(PyodideMetadataReader) {
    JSG_METHOD(isWorkerd);
    JSG_METHOD(isTracing);
    JSG_METHOD(getMainModule);
    JSG_METHOD(getNames);
    JSG_METHOD(getSizes);
    JSG_METHOD(read);
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(shouldSnapshotToDisk);
    JSG_METHOD(getPyodideVersion);
    JSG_METHOD(getPackagesVersion);
    JSG_METHOD(getPackagesLock);
    JSG_METHOD(isCreatingBaselineSnapshot);
    JSG_METHOD(getCompatibilityFlags);
    JSG_STATIC_METHOD(getBaselineSnapshotImports);
    JSG_METHOD(setCpuLimitNearlyExceededCallback);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("mainModule", state->mainModule);
    for (const auto& name: state->moduleInfo.names) {
      tracker.trackField("name", name);
    }
    for (const auto& content: state->moduleInfo.contents) {
      tracker.trackField("content", content);
    }
  }

 private:
  kj::Own<State> state;
};

struct MemorySnapshotResult {
  kj::Array<kj::byte> snapshot;
  kj::Array<kj::String> importedModulesList;
  kj::String snapshotType;
  JSG_STRUCT(snapshot, importedModulesList, snapshotType);
};

// This used to be declared nested as ArtifactBundler::State, but then there was a need to
// forward-declare it, so here we are.
struct ArtifactBundler_State {
  // The storedSnapshot is only used while isValidating is true.
  kj::Maybe<MemorySnapshotResult> storedSnapshot;

  // A memory snapshot of the state of the Python interpreter after initialization. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<const kj::byte>> existingSnapshot;

  // Set only when the validator is running. This is used to determine if it is appropriate
  // to store a memory snapshot.
  bool isValidating;

  // Set when the worker is a dynamically-loaded worker. Dynamic workers don't support dedicated
  // snapshots yet, so the Python runtime uses this to skip snapshot type validation.
  bool isDynamicWorkerFlag;

  ArtifactBundler_State(kj::Maybe<kj::Array<const kj::byte>> existingSnapshot,
      bool isValidating = false,
      bool isDynamicWorker = false)
      : storedSnapshot(kj::none),
        existingSnapshot(kj::mv(existingSnapshot)),
        isValidating(isValidating),
        isDynamicWorkerFlag(isDynamicWorker) {};

  kj::Own<ArtifactBundler_State> clone() {
    return kj::heap<ArtifactBundler_State>(
        existingSnapshot.map(
            [](kj::Array<const kj::byte>& data) { return kj::heapArray<const kj::byte>(data); }),
        isValidating, isDynamicWorkerFlag);
  }
};

// A loaded bundle of artifacts for a particular script id. It can also contain V8 version and
// CPU architecture-specific artifacts. The logic for loading these is in getArtifacts.
class ArtifactBundler: public jsg::Object {
 public:
  using State = ArtifactBundler_State;

  ArtifactBundler(kj::Own<State> inner): inner(kj::mv(inner)) {};

  void storeMemorySnapshot(jsg::Lock& js, MemorySnapshotResult snapshot) {
    KJ_REQUIRE(inner->isValidating);
    inner->storedSnapshot = kj::mv(snapshot);
  }

  bool hasMemorySnapshot() {
    return inner->existingSnapshot != kj::none;
  }

  uint32_t getMemorySnapshotSize() {
    if (inner->existingSnapshot == kj::none) {
      return 0;
    }
    auto size = KJ_REQUIRE_NONNULL(inner->existingSnapshot).size();
    KJ_REQUIRE(size <= static_cast<uint32_t>(kj::maxValue), "memory snapshot too large");
    return size;
  }

  uint32_t readMemorySnapshot(uint64_t offset, kj::Array<kj::byte> buf);
  void disposeMemorySnapshot() {
    inner->existingSnapshot = kj::none;
  }

  // Determines whether this ArtifactBundler was created inside the validator.
  bool isEwValidating() {
    return inner->isValidating;
  }

  // Determines whether this ArtifactBundler belongs to a dynamically-loaded worker.
  bool isDynamicWorker() {
    return inner->isDynamicWorkerFlag;
  }

  static kj::Own<State> makeDisabledBundler() {
    return kj::heap<State>(kj::none);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_IF_SOME(snap, inner->existingSnapshot) {
      tracker.trackFieldWithSize("snapshot", snap.size());
    }
  }

  bool isEnabled() {
    return false;  // TODO(later): Remove this function once we regenerate the bundle.
  }

  JSG_RESOURCE_TYPE(ArtifactBundler) {
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(isEwValidating);
    JSG_METHOD(isDynamicWorker);
    JSG_METHOD(storeMemorySnapshot);
    JSG_METHOD(isEnabled);
  }

 private:
  kj::Own<State> inner;
};

class DisabledInternalJaeger: public jsg::Object {
 public:
  static jsg::Ref<DisabledInternalJaeger> create(jsg::Lock& js) {
    return js.alloc<DisabledInternalJaeger>();
  }
  JSG_RESOURCE_TYPE(DisabledInternalJaeger) {}
};

// This cache is used by Pyodide to store wheels fetched over the internet across workerd restarts in local dev only
class DiskCache: public jsg::Object {
 private:
  static const kj::Maybe<kj::Own<const kj::Directory>> NULL_CACHE_ROOT;  // always set to kj::none

  const kj::Maybe<kj::Own<const kj::Directory>>& cacheRoot;
  const kj::Maybe<kj::Own<const kj::Directory>>& snapshotRoot;

 public:
  DiskCache(): cacheRoot(NULL_CACHE_ROOT), snapshotRoot(NULL_CACHE_ROOT) {};  // Disabled disk cache
  DiskCache(const kj::Maybe<kj::Own<const kj::Directory>>& cacheRoot,
      const kj::Maybe<kj::Own<const kj::Directory>>& snapshotRoot)
      : cacheRoot(cacheRoot),
        snapshotRoot(snapshotRoot) {};

  jsg::Optional<kj::Array<kj::byte>> get(jsg::Lock& js, kj::String key);
  void put(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data);
  void putSnapshot(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data);

  JSG_RESOURCE_TYPE(DiskCache) {
    JSG_METHOD(get);
    JSG_METHOD(put);
    JSG_METHOD(putSnapshot);
  }
};

// Reports worker fatal errors to the request observer for Runtime Analytics.
// This is exposed to the Python runtime as a module so that the on_fatal callback
// can report fatal errors.
class WorkerFatalReporter: public jsg::Object {
 public:
  WorkerFatalReporter() {}

  void reportFatal(jsg::Lock& js, kj::String error);
  void reportPythonWorkersInternalError(jsg::Lock& js);

  JSG_RESOURCE_TYPE(WorkerFatalReporter) {
    JSG_METHOD(reportFatal);
    JSG_METHOD(reportPythonWorkersInternalError);
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

  static jsg::Ref<SimplePythonLimiter> makeDisabled(jsg::Lock& js) {
    return js.alloc<SimplePythonLimiter>();
  }

  void beginStartup() {
    KJ_IF_SOME(cb, getTimeCb) {
      JSG_REQUIRE(startTime == kj::none, TypeError, "Cannot call `beginStartup` multiple times.");
      startTime = cb();
    }
  }

  void finishStartup(kj::Maybe<kj::String> snapshotType) {
    KJ_IF_SOME(cb, getTimeCb) {
      JSG_REQUIRE(startTime != kj::none, TypeError, "Need to call `beginStartup` first.");
      auto endTime = cb();
      kj::Duration diff = endTime - KJ_ASSERT_NONNULL(startTime);
      auto diffMs = diff / kj::MILLISECONDS;

      JSG_REQUIRE(diffMs <= startupLimitMs, TypeError, "Python Worker startup exceeded CPU limit ",
          diffMs, "<=", startupLimitMs, " with snapshot ", snapshotType.orDefault(kj::str("none")));
    }
  }

  JSG_RESOURCE_TYPE(SimplePythonLimiter) {
    JSG_METHOD(beginStartup);
    JSG_METHOD(finishStartup);
  }
};

kj::Maybe<kj::String> getPyodideLock(PythonSnapshotRelease::Reader pythonSnapshotRelease);

// Computes the subresource-integrity-style checksum ("sha256-<base64>") of the given bytes.
kj::String computePyodideBundleIntegrity(kj::ArrayPtr<const kj::byte> bytes);

// Verifies that a fetched/downloaded Pyodide bundle matches the expected subresource-integrity
// checksum from the release metadata. Throws on mismatch. Verification is skipped only for the
// "dev" bundle (built locally, no published checksum); for any other bundle a blank
// `expectedIntegrity` is itself an error.
void verifyPyodideBundleIntegrity(
    kj::StringPtr version, kj::StringPtr expectedIntegrity, kj::ArrayPtr<const kj::byte> bytes);

#define EW_PYODIDE_ISOLATE_TYPES                                                                   \
  api::pyodide::ReadOnlyBuffer, api::pyodide::PythonPackageFileMetadata,                           \
      api::pyodide::EmbeddedPackagesReader, api::pyodide::PyodideMetadataReader,                   \
      api::pyodide::ArtifactBundler, api::pyodide::DiskCache,                                      \
      api::pyodide::DisabledInternalJaeger, api::pyodide::SimplePythonLimiter,                     \
      api::pyodide::WorkerFatalReporter, api::pyodide::MemorySnapshotResult

}  // namespace workerd::api::pyodide

namespace workerd {
kj::Maybe<PythonSnapshotRelease::Reader> getPythonSnapshotRelease(
    CompatibilityFlags::Reader featureFlags);
kj::String getPythonBundleName(PythonSnapshotRelease::Reader pyodideRelease);
}  // namespace workerd
