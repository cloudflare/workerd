#pragma once

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
  ArtifactBundler(kj::Maybe<kj::Array<kj::byte>> existingSnapshot,
      kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)> uploadMemorySnapshotCb)
      : existingSnapshot(kj::mv(existingSnapshot)),
        uploadMemorySnapshotCb(kj::mv(uploadMemorySnapshotCb)),
        hasUploaded(false) {};

  ArtifactBundler(kj::Maybe<kj::Array<kj::byte>> existingSnapshot)
      : existingSnapshot(kj::mv(existingSnapshot)),
        uploadMemorySnapshotCb(kj::none),
        hasUploaded(false) {};

  ArtifactBundler()
      : existingSnapshot(kj::none),
        uploadMemorySnapshotCb(kj::none),
        hasUploaded(false) {};

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
    return context.awaitIo(cb(kj::mv(snapshot)));
  };

  jsg::Optional<kj::Array<kj::byte>> getMemorySnapshot(jsg::Lock& js) {
    KJ_IF_SOME(val, existingSnapshot) {
      return kj::mv(val);
    }
    return kj::none;
  }

  bool isEnabled() {
    return uploadMemorySnapshotCb != kj::none;
  }

  static jsg::Ref<ArtifactBundler> makeDisabledBundler() {
    return jsg::alloc<ArtifactBundler>();
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_IF_SOME(snapshot, existingSnapshot) {
      tracker.trackFieldWithSize("snapshot", snapshot.size());
    }
  }

  JSG_RESOURCE_TYPE(ArtifactBundler) {
    JSG_METHOD(uploadMemorySnapshot);
    JSG_METHOD(getMemorySnapshot);
    JSG_METHOD(isEnabled);
  }
private:
  // A memory snapshot of the state of the Python interpreter after initialisation. Used to speed
  // up cold starts.
  kj::Maybe<kj::Array<kj::byte>> existingSnapshot;
  kj::Maybe<kj::Function<kj::Promise<bool>(kj::Array<kj::byte> snapshot)>> uploadMemorySnapshotCb;
  bool hasUploaded;
};

using Worker = server::config::Worker;

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf);

#define EW_PYODIDE_ISOLATE_TYPES       \
  api::pyodide::PackagesTarReader,     \
  api::pyodide::PyodideMetadataReader, \
  api::pyodide::ArtifactBundler

template <class Registry> void registerPyodideModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getPythonWorkers()) {
    // We add `pyodide:` packages here including python-entrypoint-helper.js.
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
    registry.template addBuiltinModule<PackagesTarReader>(
        "pyodide-internal:packages_tar_reader", workerd::jsg::ModuleRegistry::Type::INTERNAL);
  }
}

} // namespace workerd::api::pyodide
