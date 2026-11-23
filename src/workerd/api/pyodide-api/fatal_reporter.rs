// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use io_context::IoContext;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::ffi;

/// `pyodide-internal:fatal-reporter`, through which the Python runtime reports its fatal and
/// internal errors to the metrics of the current request, for Runtime Analytics.
///
/// Outside a request, or when recording fails, a report does nothing.
#[jsg_resource]
#[derive(Default)]
pub struct WorkerFatalReporter;

#[jsg_resource]
impl WorkerFatalReporter {
    /// Reports that the Worker hit a fatal error, which `error` describes. Only the report is
    /// recorded, not `error`, which is required and converted to a string, as a C++ `kj::String`
    /// parameter is.
    #[jsg_method]
    pub fn report_fatal<'s>(&self, lock: &mut jsg::Lock<'s>, _error: jsg::JsString<'s>) {
        if let Some(io_context) = IoContext::try_current(lock) {
            ffi::report_worker_fatal(io_context.as_ffi());
        }
    }

    /// Reports that the Python runtime hit an internal error.
    #[jsg_method]
    pub fn report_python_workers_internal_error(&self, lock: &mut jsg::Lock<'_>) {
        if let Some(io_context) = IoContext::try_current(lock) {
            ffi::report_python_workers_internal_error(io_context.as_ffi());
        }
    }
}

#[cfg(test)]
#[path = "fatal_reporter-test.rs"]
mod tests;
