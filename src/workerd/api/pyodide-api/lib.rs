// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Resources that the Pyodide runtime's JavaScript (`src/pyodide/internal`) imports as
//! `pyodide-internal:` modules. Each does what the C++ class of the same name in
//! `src/workerd/api/pyodide/pyodide.h` does, and `registerPythonCommonModules()`
//! (`io/worker-modules.h`) installs it in that class's place when the `RUST_PYODIDE_BRIDGE`
//! autogate is on.

mod fatal_reporter;
mod ffi;
mod jaeger;
mod limiter;

pub use fatal_reporter::WorkerFatalReporter;
pub use jaeger::DisabledInternalJaeger;
pub use limiter::SimplePythonLimiter;
