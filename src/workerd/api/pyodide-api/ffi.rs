// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ registers the modules of this crate, and the C++ they call.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

use std::pin::Pin;

pub use bridge::report_python_workers_internal_error;
pub use bridge::report_worker_fatal;
use jsg::modules::ModuleType;

use crate::DisabledInternalJaeger;
use crate::SimplePythonLimiter;
use crate::WorkerFatalReporter;

#[cxx::bridge(namespace = "workerd::api::pyodide")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }

    unsafe extern "C++" {
        include!("workerd/api/pyodide-api/ffi.h");

        /// Records in the metrics of the current request, whose `IoContext` `io_context_id`
        /// identifies, that the Worker hit a fatal error. Throws no ordinary exception: only a
        /// `kj::CanceledException`, which unwinds as a panic.
        #[cxx_name = "reportWorkerFatal"]
        fn report_worker_fatal(io_context_id: u64);

        /// Records in the metrics of the current request, whose `IoContext` `io_context_id`
        /// identifies, that the Python runtime hit an internal error. Throws no ordinary
        /// exception: only a `kj::CanceledException`, which unwinds as a panic.
        #[cxx_name = "reportPythonWorkersInternalError"]
        fn report_python_workers_internal_error(io_context_id: u64);
    }

    extern "Rust" {
        /// Registers `pyodide-internal:internalJaeger` as a [`DisabledInternalJaeger`].
        fn register_disabled_internal_jaeger(registry: Pin<&mut ModuleRegistry>);

        /// Registers `pyodide-internal:limiter` as a [`SimplePythonLimiter`] that limits
        /// nothing.
        fn register_disabled_limiter(registry: Pin<&mut ModuleRegistry>);

        /// Registers `pyodide-internal:fatal-reporter` as a [`WorkerFatalReporter`].
        fn register_fatal_reporter(registry: Pin<&mut ModuleRegistry>);
    }
}

fn register_disabled_internal_jaeger(registry: Pin<&mut bridge::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "pyodide-internal:internalJaeger",
        |lock| Ok(jsg::Rc::new(lock, DisabledInternalJaeger)),
        ModuleType::Internal,
    );
}

fn register_disabled_limiter(registry: Pin<&mut bridge::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "pyodide-internal:limiter",
        |lock| Ok(SimplePythonLimiter::disabled(lock)),
        ModuleType::Internal,
    );
}

fn register_fatal_reporter(registry: Pin<&mut bridge::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "pyodide-internal:fatal-reporter",
        |lock| Ok(jsg::Rc::new(lock, WorkerFatalReporter)),
        ModuleType::Internal,
    );
}
