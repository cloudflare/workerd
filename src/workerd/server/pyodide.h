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

// Used to preload the Pyodide bundle during workerd startup.
//
// `integrity` is a subresource-integrity-style checksum ("sha256-<base64>") used to verify the
// integrity of the bundle when downloaded from the network. It may be empty (e.g. for the "dev"
// version), in which case no verification is performed.
kj::Promise<kj::Maybe<jsg::Bundle::Reader>> fetchPyodideBundle(
    const api::pyodide::PythonConfig& pyConfig,
    kj::String version,
    kj::StringPtr integrity,
    kj::Network& network,
    kj::Timer& timer);

// Preloads the Python stdlib packages (every package in the pre-filtered lock file) for a worker.
kj::Promise<void> fetchPyodideStdlib(const api::pyodide::PythonConfig& pyConfig,
    const api::pyodide::PyodidePackageManager& pyodidePackageManager,
    workerd::PythonSnapshotRelease::Reader pythonSnapshotRelease,
    kj::Network& network,
    kj::Timer& timer);

}  // namespace workerd::server
