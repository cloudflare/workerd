// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use jsg::ToJS;

use crate::dns::DnsUtil;
use crate::inspector::InspectorModule;
use crate::url::UrlUtil;

pub mod dns;
pub mod inspector;
pub mod url;

#[cxx::bridge(namespace = "workerd::rust::api")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
        type Isolate = jsg::v8::ffi::Isolate;
    }
    unsafe extern "C++" {
        include!("workerd/rust/api/inspector.h");

        type InspectorSession;

        pub fn inspector_is_multi_tenant_process() -> bool;
        /// # Safety
        /// `isolate` must be locked by this thread, and created by a `Worker::Isolate`.
        pub unsafe fn inspector_isolate_has_v8_inspector(isolate: *mut Isolate) -> Result<bool>;
        /// # Safety
        /// `isolate` must be locked by this thread, with a context entered.
        pub unsafe fn inspector_session_connect(
            isolate: *mut Isolate,
        ) -> Result<KjOwn<InspectorSession>>;

        // A session is only reachable through `InspectorConnection` methods, which run with its
        // isolate locked and a context entered, so these need not be `unsafe`.
        pub fn inspector_session_dispatch(
            session: Pin<&mut InspectorSession>,
            message: &str,
        ) -> Result<()>;
        pub fn inspector_session_take_messages(session: Pin<&mut InspectorSession>) -> Vec<String>;
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);
    }
}

pub fn register_nodejs_modules(mut registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry.as_mut(),
        "node-internal:dns",
        |lock| DnsUtil::new().to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
    // `node-internal:inspector` is always registered: it is imported by the `node:inspector`
    // TypeScript layer, so it must resolve whenever that layer loads. A `Connection` can only be
    // created when the experimental `enable_nodejs_inspector_local_dev` flag created the isolate's
    // V8 inspector, and never in a multi-tenant process.
    jsg::modules::add_builtin(
        registry.as_mut(),
        "node-internal:inspector",
        |lock| InspectorModule::new().to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
    jsg::modules::add_builtin(
        registry,
        "node-internal:url",
        |lock| UrlUtil::new().to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
