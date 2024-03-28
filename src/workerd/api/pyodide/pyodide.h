#pragma once

#include "kj/array.h"
#include "kj/debug.h"
#include <kj/common.h>
#include <pyodide/generated/pyodide_extra.capnp.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/autogate.h>
#include <workerd/io/io-context.h>

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
  bool isTracingFlag;
  bool createBaselineSnapshot;
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot;

public:
  PyodideMetadataReader(kj::String mainModule, kj::Array<kj::String> names,
                        kj::Array<kj::Array<kj::byte>> contents, kj::Array<kj::String> requirements,
                        bool isWorkerd, bool isTracing,
                        bool createBaselineSnapshot,
                        kj::Maybe<kj::Array<kj::byte>> memorySnapshot)
      : mainModule(kj::mv(mainModule)), names(kj::mv(names)), contents(kj::mv(contents)),
        requirements(kj::mv(requirements)), isWorkerdFlag(isWorkerd), isTracingFlag(isTracing),
        createBaselineSnapshot(createBaselineSnapshot),
        memorySnapshot(kj::mv(memorySnapshot)) {}

  bool isWorkerd() {
    return this->isWorkerdFlag;
  }

  bool isTracing() {
    return this->isTracingFlag;
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

// A loaded bundle of artifacts for a particular script id. It can also contain V8 version and
// CPU architecture-specific artifacts. The logic for loading these is in getArtifacts.
class ArtifactBundler : public jsg::Object {
public:
  kj::Maybe<kj::Array<kj::byte>> storedSnapshot;

  ArtifactBundler(kj::Maybe<kj::Array<kj::byte>> existingSnapshot,
      kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)> uploadMemorySnapshotCb)
      :
        storedSnapshot(kj::none),
        existingSnapshot(kj::mv(existingSnapshot)),
        uploadMemorySnapshotCb(kj::mv(uploadMemorySnapshotCb)),
        hasUploaded(false),
        isValidating(false),
        createBaselineSnapshot(false) {};

  ArtifactBundler(kj::Maybe<kj::Array<kj::byte>> existingSnapshot)
      : storedSnapshot(kj::none),
        existingSnapshot(kj::mv(existingSnapshot)),
        uploadMemorySnapshotCb(kj::none),
        hasUploaded(false),
        isValidating(false),
        createBaselineSnapshot(false) {};

  ArtifactBundler(bool isValidating = false)
      : storedSnapshot(kj::none),
        existingSnapshot(kj::none),
        uploadMemorySnapshotCb(kj::none),
        hasUploaded(false),
        isValidating(isValidating),
        createBaselineSnapshot(false) {};

  jsg::Promise<bool> uploadMemorySnapshot(jsg::Lock& js, kj::Array<kj::byte> snapshot) {
    // Prevent multiple uploads.
    if (hasUploaded) {
      return js.rejectedPromise<bool>(
          js.typeError("This ArtifactBundle has already uploaded a memory snapshot"));
    }

    // TODO(later): Only upload if `snapshot` isn't identical to `existingSnapshot`.

    if (uploadMemorySnapshotCb == kj::none) {
      return js.rejectedPromise<bool>(js.typeError("ArtifactBundler is disabled"));
    }
    auto& cb = KJ_REQUIRE_NONNULL(uploadMemorySnapshotCb);
    hasUploaded = true;
    auto& context = IoContext::current();
    return context.awaitIo(js, cb(kj::mv(snapshot)));
  };

  void storeMemorySnapshot(jsg::Lock& js, kj::Array<kj::byte> snapshot) {
    KJ_REQUIRE(isValidating);
    storedSnapshot = kj::mv(snapshot);
  }

  bool isEnabled() {
    return uploadMemorySnapshotCb != kj::none;
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
    return jsg::alloc<ArtifactBundler>();
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    if (existingSnapshot == kj::none) {
      return;
    }
    tracker.trackFieldWithSize("snapshot", KJ_REQUIRE_NONNULL(existingSnapshot).size());
  }

  JSG_RESOURCE_TYPE(ArtifactBundler) {
    JSG_METHOD(uploadMemorySnapshot);
    JSG_METHOD(hasMemorySnapshot);
    JSG_METHOD(getMemorySnapshotSize);
    JSG_METHOD(readMemorySnapshot);
    JSG_METHOD(disposeMemorySnapshot);
    JSG_METHOD(isEnabled);
    JSG_METHOD(isEwValidating);
    JSG_METHOD(storeMemorySnapshot);
  }

private:
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<kj::byte>> existingSnapshot;
  kj::Maybe<kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)>> uploadMemorySnapshotCb;
  bool hasUploaded;
  bool isValidating;
  bool createBaselineSnapshot;
};


class DisabledInternalJaeger : public jsg::Object {
public:
  static jsg::Ref<DisabledInternalJaeger> create() {
    return jsg::alloc<DisabledInternalJaeger>();
  }
  JSG_RESOURCE_TYPE(DisabledInternalJaeger) {
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
  SimplePythonLimiter(int startupLimitMs, kj::Function<kj::TimePoint()> getTimeCb) :
      startupLimitMs(startupLimitMs),
      getTimeCb(kj::mv(getTimeCb)) {}

  SimplePythonLimiter() :
      startupLimitMs(0),
      getTimeCb(kj::none) {}

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

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf);

#define EW_PYODIDE_ISOLATE_TYPES       \
  api::pyodide::PackagesTarReader,     \
  api::pyodide::PyodideMetadataReader, \
  api::pyodide::ArtifactBundler,       \
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

} // namespace workerd::api::pyodide
