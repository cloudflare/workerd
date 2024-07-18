#pragma once

#include "kj/array.h"
#include "kj/debug.h"
#include <kj/common.h>
#include <kj/filesystem.h>
#include <pyodide/generated/pyodide_extra.capnp.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/io/io-context.h>

namespace workerd::api::pyodide {

struct PythonConfig {
  kj::Maybe<kj::Own<const kj::Directory>> diskCacheRoot;
  bool createSnapshot;
  bool createBaselineSnapshot;
};

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
  bool isTracingFlag;
  bool snapshotToDisk;
  bool createBaselineSnapshot;
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot;

public:
  PyodideMetadataReader(kj::String mainModule, kj::Array<kj::String> names,
                        kj::Array<kj::Array<kj::byte>> contents, kj::Array<kj::String> requirements,
                        bool isWorkerd, bool isTracing,
                        bool snapshotToDisk,
                        bool createBaselineSnapshot,
                        kj::Maybe<kj::Array<kj::byte>> memorySnapshot)
      : mainModule(kj::mv(mainModule)), names(kj::mv(names)), contents(kj::mv(contents)),
        requirements(kj::mv(requirements)), isWorkerdFlag(isWorkerd), isTracingFlag(isTracing),
        snapshotToDisk(snapshotToDisk),
        createBaselineSnapshot(createBaselineSnapshot),
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

  JSG_RESOURCE_TYPE(PyodideMetadataReader) {
    JSG_METHOD(isWorkerd);
    JSG_METHOD(isTracing);
    JSG_METHOD(getMainModule);
    JSG_METHOD(getRequirements);
    JSG_METHOD(getNames);
    JSG_METHOD(getSizes);
    JSG_METHOD(read);
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(shouldSnapshotToDisk);
    JSG_METHOD(isCreatingBaselineSnapshot);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("mainModule", mainModule);
    for (const auto& name : names) {
      tracker.trackField("name", name);
    }
    for (const auto& content : contents) {
      tracker.trackField("content", content);
    }
    for (const auto& requirement : requirements) {
      tracker.trackField("requirement", requirement);
    }
  }
};

class ValidatorSnapshotUploader : public jsg::Object {
public:
  ValidatorSnapshotUploader()
      :
        storedSnapshot(kj::none) {}


  void storeMemorySnapshot(jsg::Lock& js, kj::Array<kj::byte> snapshot) {
    storedSnapshot = kj::mv(snapshot);
  }

  JSG_RESOURCE_TYPE(ValidatorSnapshotUploader) {
    JSG_METHOD(storeMemorySnapshot);
  }
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<kj::byte>> storedSnapshot;
};

class RuntimeSnapshotUploader : public jsg::Object {
public:
  RuntimeSnapshotUploader(kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)> uploadMemorySnapshotCb)
      :
        uploadMemorySnapshotCb(kj::mv(uploadMemorySnapshotCb)),
        hasUploaded(false) {};

  jsg::Promise<bool> uploadMemorySnapshot(jsg::Lock& js, kj::Array<kj::byte> snapshot) {
    // Prevent multiple uploads.
    if (hasUploaded) {
      return js.rejectedPromise<bool>(
          js.typeError("This RuntimeArtifactUploader has already uploaded a memory snapshot"));
    }

    if (uploadMemorySnapshotCb == kj::none) {
      return js.rejectedPromise<bool>(js.typeError("RuntimeArtifactUploader is disabled"));
    }
    auto& cb = KJ_REQUIRE_NONNULL(uploadMemorySnapshotCb);
    hasUploaded = true;
    auto& context = IoContext::current();
    return context.awaitIo(js, cb(kj::mv(snapshot)));
  };

  JSG_RESOURCE_TYPE(RuntimeSnapshotUploader) {
    JSG_METHOD(uploadMemorySnapshot);
  }
private:
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)>> uploadMemorySnapshotCb;
  bool hasUploaded;
};

// A loaded bundle of artifacts for a particular script id. It can also contain V8 version and
// CPU architecture-specific artifacts. The logic for loading these is in getArtifacts.
class SnapshotDownloader : public jsg::Object {
public:
  SnapshotDownloader(kj::Maybe<kj::Array<kj::byte>> snapshot)
      :
        snapshot(kj::mv(snapshot)) {};


  bool hasMemorySnapshot() {
    return snapshot != kj::none;
  }

  int getMemorySnapshotSize() {
    if (snapshot == kj::none) {
      return 0;
    }
    return KJ_REQUIRE_NONNULL(snapshot).size();
  }

  int readMemorySnapshot(int offset, kj::Array<kj::byte> buf);
  void disposeMemorySnapshot() {
    snapshot = kj::none;
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    if (snapshot == kj::none) {
      return;
    }
    tracker.trackFieldWithSize("snapshot", KJ_REQUIRE_NONNULL(snapshot).size());
  }

  JSG_RESOURCE_TYPE(SnapshotDownloader) {
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
  }

private:
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<kj::byte>> snapshot;
};

struct Artifacts {
  kj::Maybe<jsg::Ref<SnapshotDownloader>> snapshotDownloader;
  kj::Maybe<jsg::Ref<RuntimeSnapshotUploader>> runtimeSnapshotUploader;
  kj::Maybe<jsg::Ref<ValidatorSnapshotUploader>> validatorSnapshotUploader;
  JSG_STRUCT(snapshotDownloader, runtimeSnapshotUploader, validatorSnapshotUploader);

private:
  Artifacts(kj::Maybe<jsg::Ref<SnapshotDownloader>> snapshotDownloader, kj::Maybe<jsg::Ref<RuntimeSnapshotUploader>> runtimeSnapshotUploader, kj::Maybe<jsg::Ref<ValidatorSnapshotUploader>> validatorSnapshotUploader) :
    snapshotDownloader(kj::mv(snapshotDownloader)),
    runtimeSnapshotUploader(kj::mv(runtimeSnapshotUploader)),
    validatorSnapshotUploader(kj::mv(validatorSnapshotUploader)) {}

public:
  Artifacts(Artifacts const& copy) = delete;
  Artifacts(Artifacts&& copy) = default;

  static Artifacts disabled() {
    return Artifacts(kj::none, kj::none, kj::none);
  }

  static Artifacts validator(jsg::Ref<ValidatorSnapshotUploader> uploader) {
    return Artifacts(kj::none, kj::none, kj::mv(uploader));
  }

  static Artifacts runtimeSnapshotHandler(jsg::Ref<SnapshotDownloader> snapshotDownloader, kj::Maybe<jsg::Ref<RuntimeSnapshotUploader>> snapshotUploader) {
    return Artifacts(kj::mv(snapshotDownloader), kj::mv(snapshotUploader), kj::none);
  }

};

class DisabledInternalJaeger : public jsg::Object {
public:
  static jsg::Ref<DisabledInternalJaeger> create() {
    return jsg::alloc<DisabledInternalJaeger>();
  }
  JSG_RESOURCE_TYPE(DisabledInternalJaeger) {
  }
};

// This cache is used by Pyodide to store wheels fetched over the internet across workerd restarts in local dev only
class DiskCache: public jsg::Object {
private:
  static const kj::Maybe<kj::Own<const kj::Directory>> NULL_CACHE_ROOT; // always set to kj::none

  const kj::Maybe<kj::Own<const kj::Directory>> &cacheRoot;

public:
  DiskCache(): cacheRoot(NULL_CACHE_ROOT) {}; // Disabled disk cache
  DiskCache(const kj::Maybe<kj::Own<const kj::Directory>> &cacheRoot): cacheRoot(cacheRoot) {};

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
class SimplePythonLimiter : public jsg::Object {
private:
  int startupLimitMs;
  kj::Maybe<kj::Function<kj::TimePoint()>> getTimeCb;

  kj::Maybe<kj::TimePoint> startTime;



public:
  SimplePythonLimiter() :
      startupLimitMs(0),
      getTimeCb(kj::none) {}

  SimplePythonLimiter(int startupLimitMs, kj::Function<kj::TimePoint()> getTimeCb) :
      startupLimitMs(startupLimitMs),
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

using Worker = server::config::Worker;

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf, const PythonConfig& pythonConfig);

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules);

#define EW_PYODIDE_ISOLATE_TYPES       \
  api::pyodide::PackagesTarReader,     \
  api::pyodide::PyodideMetadataReader, \
  api::pyodide::Artifacts, \
  api::pyodide::ValidatorSnapshotUploader,       \
  api::pyodide::RuntimeSnapshotUploader,       \
  api::pyodide::SnapshotDownloader,       \
  api::pyodide::DiskCache,             \
  api::pyodide::DisabledInternalJaeger,\
  api::pyodide::SimplePythonLimiter

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getPythonWorkers()) {
    // We add `pyodide:` packages here including python-entrypoint-helper.js.
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
    registry.template addBuiltinModule<PackagesTarReader>(
        "pyodide-internal:packages_tar_reader", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  }
}

kj::Own<jsg::modules::ModuleBundle> getInternalPyodideModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, PYODIDE_BUNDLE);
  return builder.finish();
}

kj::Own<jsg::modules::ModuleBundle> getExternalPyodideModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, PYODIDE_BUNDLE);
  return builder.finish();
}

} // namespace workerd::api::pyodide
