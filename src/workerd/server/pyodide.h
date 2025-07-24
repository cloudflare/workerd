// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/pyodide/pyodide.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

#include <kj/array.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/string.h>
#include <kj/timer.h>

namespace workerd::server {

// Used to preload the Pyodide bundle during workerd startup
kj::Promise<kj::Maybe<jsg::Bundle::Reader>> fetchPyodideBundle(
    const api::pyodide::PythonConfig& pyConfig,
    kj::String version,
    kj::Network& network,
    kj::Timer& timer);

// Preloads all required Python packages for a worker
kj::Promise<void> fetchPyodidePackages(const api::pyodide::PythonConfig& pyConfig,
    const api::pyodide::PyodidePackageManager& pyodidePackageManager,
    kj::ArrayPtr<kj::String> pythonRequirements,
    workerd::PythonSnapshotRelease::Reader pythonSnapshotRelease,
    kj::Network& network,
    kj::Timer& timer);

}  // namespace workerd::server
