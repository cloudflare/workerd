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
    KJ_ASSERT(res.getPyodide() == "0.28.2");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20250116");
  }

  featureFlags.setPythonWorkers20260610(true);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "314.0.3");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20260610");
  }

  featureFlags.setPythonWorkersDevPyodide(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "314.0.3");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20260610");
  }

  featureFlags.setPythonWorkers20260610(false);
  {
    auto res = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    KJ_ASSERT(res.getPyodide() == "0.28.2");
    KJ_ASSERT(res.getFlagName() == "pythonWorkers20250116");
  }
}

template <typename... Params>
kj::Array<kj::String> strArray(Params&&... params) {
  return kj::arr(kj::str(params)...);
}

template <typename... Params>
kj::Array<kj::Array<kj::byte>> bytesArray(Params&&... params) {
  return kj::arr(kj::heapArray<kj::byte>(kj::str(params).asBytes())...);
}

template <typename... Params>
kj::HashSet<kj::String> strSet(Params&&... params) {
  auto array = strArray(params...);
  kj::HashSet<kj::String> set;
  for (auto& str: array) {
    set.insert(kj::mv(str));
  }
  return set;
}

KJ_TEST("computePyodideBundleIntegrity produces sha256 subresource-integrity strings") {
  // Known-answer test: SHA-256 of the empty input.
  KJ_EXPECT(pyodide::computePyodideBundleIntegrity(kj::ArrayPtr<const kj::byte>()) ==
      "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");

  // SHA-256 of "abc".
  auto abc = "abc"_kj.asBytes();
  KJ_EXPECT(pyodide::computePyodideBundleIntegrity(abc) ==
      "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=");
}

KJ_TEST("verifyPyodideBundleIntegrity accepts matching checksums") {
  auto data = "hello pyodide"_kj.asBytes();
  auto integrity = pyodide::computePyodideBundleIntegrity(data);

  // Should not throw when the checksum matches.
  pyodide::verifyPyodideBundleIntegrity("0.28.2"_kj, integrity, data);
}

KJ_TEST("verifyPyodideBundleIntegrity rejects a missing checksum for released bundles") {
  auto data = "hello pyodide"_kj.asBytes();

  // A non-dev bundle without a published checksum is an error.
  KJ_EXPECT_THROW_MESSAGE("missing an integrity checksum",
      pyodide::verifyPyodideBundleIntegrity("0.28.2"_kj, nullptr, data));
  KJ_EXPECT_THROW_MESSAGE("missing an integrity checksum",
      pyodide::verifyPyodideBundleIntegrity("0.28.2"_kj, ""_kj, data));
}

KJ_TEST("verifyPyodideBundleIntegrity skips the dev bundle") {
  auto data = "hello pyodide"_kj.asBytes();
  auto tampered = "hello pyodide!"_kj.asBytes();
  auto integrity = pyodide::computePyodideBundleIntegrity(data);

  // The "dev" bundle is built locally and has no published checksum, so verification is skipped
  // even when the supplied integrity does not match, and an empty integrity is allowed.
  pyodide::verifyPyodideBundleIntegrity("dev"_kj, integrity, tampered);
  pyodide::verifyPyodideBundleIntegrity("dev"_kj, nullptr, tampered);
}

KJ_TEST("verifyPyodideBundleIntegrity rejects mismatching checksums") {
  auto data = "hello pyodide"_kj.asBytes();
  auto tampered = "hello pyodide!"_kj.asBytes();
  auto integrity = pyodide::computePyodideBundleIntegrity(data);

  KJ_EXPECT_THROW_MESSAGE("integrity check failed",
      pyodide::verifyPyodideBundleIntegrity("0.28.2"_kj, integrity, tampered));
}

}  // namespace
}  // namespace workerd::api
