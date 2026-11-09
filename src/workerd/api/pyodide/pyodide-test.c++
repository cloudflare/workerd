// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "pyodide.h"

#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("getPythonSnapshotRelease") {
  capnp::MallocMessageBuilder arena;
  // TODO(beta): Factor out FeatureFlags from WorkerBundle.
  auto featureFlags = arena.initRoot<CompatibilityFlags>();

  {
    auto res = getPythonSnapshotRelease(featureFlags);
    KJ_ASSERT(res == kj::none);
  }

  featureFlags.setPythonWorkers(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "0.26.0a2");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers");
    // The bundle integrity checksum is plumbed through from python_metadata.bzl.
  }

  featureFlags.setPythonWorkersDevPyodide(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "dev");
    KJ_ASSERT(res.getFlagName() == "pythonWorkersDevPyodide");
  }

  featureFlags.setPythonWorkers(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "dev");
    KJ_ASSERT(res.getFlagName() == "pythonWorkersDevPyodide");
  }

  featureFlags.setPythonWorkers20250116(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "dev");
    KJ_ASSERT(res.getFlagName() == "pythonWorkersDevPyodide");
  }

  featureFlags.setPythonWorkers20260610(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "dev");
    KJ_ASSERT(res.getFlagName() == "pythonWorkersDevPyodide");
  }

  featureFlags.setPythonWorkers314(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "dev");
    KJ_ASSERT(res.getFlagName() == "pythonWorkersDevPyodide");
  }

  featureFlags.setPythonWorkersDevPyodide(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "314.0.6");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers314");
  }

  featureFlags.setPythonWorkers314(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "314.0.4");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20260610");
  }

  featureFlags.setPythonWorkers20260610(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "0.28.2");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20250116");
  }
}

KJ_TEST("pythonV8SnapshotSupported") {
  capnp::MallocMessageBuilder arena;
  auto flags = arena.initRoot<CompatibilityFlags>();

  // Not a Python worker.
  KJ_EXPECT(!pythonV8SnapshotSupported(flags));

  // Pyodide 314.0.6 with every Python load flag that predates it on.
  flags.setPythonWorkers(true);
  flags.setPythonWorkers314(true);
  flags.setPythonProcessPthFiles(true);
  flags.setJsWeakRef(true);
  KJ_EXPECT(pythonV8SnapshotSupported(flags));

  // Flags that postdate the release, and undated ones, may be either way.
  flags.setPythonSharedStandardLibrary(true);
  KJ_EXPECT(pythonV8SnapshotSupported(flags));
  flags.setPythonSharedStandardLibrary(false);

  // Flags Pyodide reads only after the baseline do not matter.
  flags.setPythonNoGlobalHandlers(false);
  flags.setPythonExternalSDK(false);
  KJ_EXPECT(pythonV8SnapshotSupported(flags));

  // A load flag that predates the release turned off.
  flags.setPythonProcessPthFiles(false);
  KJ_EXPECT(!pythonV8SnapshotSupported(flags));
  flags.setPythonProcessPthFiles(true);

  // The development bundle tracks a supported release.
  flags.setPythonWorkersDevPyodide(true);
  KJ_EXPECT(pythonV8SnapshotSupported(flags));
  flags.setPythonWorkersDevPyodide(false);

  // Pyodide 314.0.4 is the oldest supported release; 0.28.2 is not supported.
  flags.setPythonWorkers314(false);
  flags.setPythonWorkers20260610(true);
  KJ_EXPECT(pythonV8SnapshotSupported(flags));
  flags.setPythonWorkers20260610(false);
  flags.setPythonWorkers20250116(true);
  KJ_EXPECT(!pythonV8SnapshotSupported(flags));
}

KJ_TEST("getPythonCompatibilityFlagsJson") {
  capnp::MallocMessageBuilder arena;
  auto flags = arena.initRoot<CompatibilityFlags>();
  flags.setPythonWorkers(true);
  flags.setPythonProcessPthFiles(true);
  // Flags the baseline load does not depend on do not appear.
  flags.setNodeJsCompat(true);
  flags.setPythonNoGlobalHandlers(true);
  auto json = getPythonCompatibilityFlagsJson(flags);
  KJ_EXPECT(json.startsWith("{\"python_workers\":true,"), json);
  KJ_EXPECT(strstr(json.cStr(), "\"python_process_pth_files\":true") != nullptr, json);
  KJ_EXPECT(strstr(json.cStr(), "\"python_workers_314\":false") != nullptr, json);
  KJ_EXPECT(strstr(json.cStr(), "\"enable_weak_ref\":false") != nullptr, json);
  KJ_EXPECT(strstr(json.cStr(), "nodejs") == nullptr, json);
  KJ_EXPECT(strstr(json.cStr(), "global_handlers") == nullptr, json);
}

}  // namespace
}  // namespace workerd::api
