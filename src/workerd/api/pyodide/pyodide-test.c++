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

}  // namespace
}  // namespace workerd::api
