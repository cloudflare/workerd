// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ registers the modules of this crate.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

use std::pin::Pin;

use jsg::modules::ModuleType;

use crate::DisabledInternalJaeger;
use crate::SimplePythonLimiter;

#[cxx::bridge(namespace = "workerd::api::pyodide")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }

    extern "Rust" {
        /// Registers `pyodide-internal:internalJaeger` as a [`DisabledInternalJaeger`].
        fn register_disabled_internal_jaeger(registry: Pin<&mut ModuleRegistry>);

        /// Registers `pyodide-internal:limiter` as a [`SimplePythonLimiter`] that limits
        /// nothing.
        fn register_disabled_limiter(registry: Pin<&mut ModuleRegistry>);
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
