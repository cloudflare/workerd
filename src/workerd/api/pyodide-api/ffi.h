// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <cstdint>

namespace workerd::api::pyodide {

// Record in the metrics of the current request, whose IoContext `ioContextId` identifies (see
// io-context/current-with-id.h), that the Worker hit a fatal error, or that the Python runtime
// hit an internal error. As WorkerFatalReporter's methods do, they ignore any exception that
// recording throws except kj::CanceledException, which kj::runCatchingExceptions() rethrows, and
// so throw no ordinary exception.
void reportWorkerFatal(uint64_t ioContextId);
void reportPythonWorkersInternalError(uint64_t ioContextId);

}  // namespace workerd::api::pyodide
