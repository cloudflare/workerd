#include "worker-modules.h"

namespace workerd::modules::python {
kj::Own<api::pyodide::PyodideMetadataReader::State> createPyodideMetadataState(
    const Worker::Script::ModulesSource& source,
    api::pyodide::IsWorkerd isWorkerd,
    api::pyodide::IsTracing isTracing,
    api::pyodide::SnapshotToDisk snapshotToDisk,
    api::pyodide::CreateBaselineSnapshot createBaselineSnapshot,
    PythonSnapshotRelease::Reader pythonRelease,
    kj::Maybe<kj::Array<kj::byte>> maybeSnapshot,
    CompatibilityFlags::Reader featureFlags) {
  auto mainModule = kj::str(source.mainModule);
  auto modules = source.modules.asPtr();
  int numFiles = 0;
  int numRequirements = 0;
  for (auto& module: modules) {
    KJ_SWITCH_ONEOF(module.content) {
      KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
        numFiles++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::ObsoletePythonRequirement) {
        numRequirements++;
      }
      KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
        // Not exposed to Python.
      }
    }
  }

  auto names = kj::heapArrayBuilder<kj::String>(numFiles);
  auto contents = kj::heapArrayBuilder<kj::Array<kj::byte>>(numFiles);
  auto requirements = kj::heapArrayBuilder<kj::String>(numRequirements);
  for (auto& module: modules) {
    KJ_SWITCH_ONEOF(module.content) {
      KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body));
      }
      KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
        // Not exposed to Python.
      }
      KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
        KJ_REQUIRE(module.name.endsWith(".py"));
        names.add(kj::str(module.name));
        contents.add(kj::heapArray(content.body.asBytes()));
      }
      KJ_CASE_ONEOF(content, Worker::Script::ObsoletePythonRequirement) {
        requirements.add(kj::str(module.name));
      }
      KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
        // Not exposeud to Python.
      }
    }
  }

  auto lock = KJ_ASSERT_NONNULL(workerd::api::pyodide::getPyodideLock(pythonRelease),
      kj::str("No lock file defined for Python packages release ", pythonRelease.getPackages()));

  // clang-format off
  return kj::heap<workerd::api::pyodide::PyodideMetadataReader::State>(
      kj::mv(mainModule),
      names.finish(),
      contents.finish(),
      requirements.finish(),
      kj::str(pythonRelease.getPyodide()),
      kj::str(pythonRelease.getPackages()),
      kj::mv(lock),
      isWorkerd,
      isTracing,
      snapshotToDisk,
      createBaselineSnapshot,
      kj::mv(maybeSnapshot));
  // clang-format on
}

kj::Maybe<kj::Array<kj::byte>> tryGetMetadataSnapshot(
    const api::pyodide::PythonConfig& pythonConfig, api::pyodide::SnapshotToDisk snapshotToDisk) {
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot = kj::none;
  KJ_IF_SOME(snapshot, pythonConfig.loadSnapshotFromDisk) {
    auto& root = KJ_REQUIRE_NONNULL(pythonConfig.snapshotDirectory);
    kj::Path path(snapshot);
    auto maybeFile = root->tryOpenFile(path);
    if (maybeFile == kj::none) {
      KJ_FAIL_REQUIRE("Expected to find", snapshot, "in the package cache directory");
    }
    memorySnapshot = KJ_REQUIRE_NONNULL(maybeFile)->readAllBytes();
  }
  return kj::mv(memorySnapshot);
}

jsg::Bundle::Reader retrievePyodideBundle(
    const api::pyodide::PythonConfig& pyConfig, kj::StringPtr version) {
  auto result = pyConfig.pyodideBundleManager.getPyodideBundle(version);
  return KJ_ASSERT_NONNULL(result, "Failed to get Pyodide bundle", version);
}
}  // namespace workerd::modules::python
