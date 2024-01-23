// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <pyodide/pyodide.capnp.h>
#include <kj/debug.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd::api::pyodide {

kj::StringPtr getPyodideBootstrap();

kj::StringPtr getPyodideLock();

kj::StringPtr getPyodidePatch(kj::StringPtr name);

kj::String generatePyodideMetadata(server::config::Worker::Reader conf);

kj::String generatePyodidePatches();

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules);

}  // namespace workerd
