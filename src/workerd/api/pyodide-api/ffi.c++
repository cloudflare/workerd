// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/io-context.h>
#include <workerd/io/io-context/current-with-id.h>

namespace workerd::api::pyodide {

void reportWorkerFatal(uint64_t ioContextId) {
  kj::runCatchingExceptions(
      [&]() { io_context::currentWithId(ioContextId).getMetrics().setWorkerFatal(); });
}

void reportPythonWorkersInternalError(uint64_t ioContextId) {
  kj::runCatchingExceptions([&]() {
    io_context::currentWithId(ioContextId).getMetrics().setPythonWorkersInternalError();
  });
}

}  // namespace workerd::api::pyodide
