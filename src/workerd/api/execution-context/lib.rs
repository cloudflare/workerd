// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `ExecutionContext`, the `ctx` that the runtime passes to the handlers of a stateless
//! entrypoint, and to the constructor of a `WorkerEntrypoint` or `WorkflowEntrypoint` class.
//!
//! It behaves as the C++ `api::ExecutionContext` (`src/workerd/api/global-scope.h`) does, which
//! it replaces under the `rust-execution-context` autogate. C++ holds either one as an
//! `api::exportedhandler::ExecutionContext`, and creates this one through
//! `api::newExecutionContext()` (`execution-context.h`).

mod ffi;

use ffi::bridge;
use io_context::IoContext;
use jsg::MemberRegistry;
use jsg::PropertyPlacement;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

/// The `ctx` of a request to a stateless entrypoint.
///
/// Its members are those of the C++ class, in the same order and with the same placement: the
/// methods on the prototype and the properties as lazy own data properties, gated by the same
/// compatibility flags.
#[jsg_resource(name = "ExecutionContext")]
pub struct ExecutionContext {
    /// `ctx.exports`: the Worker's loopback exports.
    exports: v8::TracedGlobal<v8::Value>,
    /// `ctx.props`: the props of the entrypoint the request was sent to.
    props: v8::TracedGlobal<v8::Value>,
    /// `ctx.version`, if the request has version information.
    version: Option<v8::TracedGlobal<v8::Value>>,
}

impl ExecutionContext {
    fn new<'s>(
        lock: &mut jsg::Lock<'s>,
        exports: v8::Local<'s, v8::Value>,
        props: v8::Local<'s, v8::Value>,
        version: Option<v8::Local<'s, v8::Value>>,
    ) -> Self {
        Self {
            exports: exports.to_traced_global(lock),
            props: props.to_traced_global(lock),
            version: version.map(|version| version.to_traced_global(lock)),
        }
    }
}

#[jsg_resource(custom_members)]
impl ExecutionContext {
    /// Has the request wait for `promise`, as `ctx.waitUntil()` does.
    #[jsg_method]
    pub fn wait_until<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: v8::VoidPromise<'s>,
    ) -> jsg::Result<()> {
        IoContext::current(lock)?.add_wait_until(lock, promise)
    }

    /// Has the request fail open.
    #[jsg_method]
    pub fn pass_through_on_exception(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
        IoContext::current(lock)?.set_fail_open()
    }

    #[jsg_property(lazy_instance)]
    pub fn get_exports<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.exports.as_local(lock)
    }

    #[jsg_property(lazy_instance)]
    pub fn get_props<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.props.as_local(lock)
    }

    /// Calls the `[restore]()` method of the current entrypoint with `params`, returning a value
    /// that can be persisted in storage; see C++ `ExecutionContext::restore()`.
    #[jsg_method]
    pub fn restore<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        params: v8::Local<'s, v8::Object>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        ffi::restore_current_entrypoint(lock, params)
    }

    /// The embedder's `CacheContext` for the current request, if it provides one.
    #[jsg_property(lazy_instance)]
    pub fn get_cache(
        &self,
        lock: &mut jsg::Lock<'_>,
    ) -> jsg::Result<Option<jsg_cpp::Ref<bridge::CacheContext>>> {
        bridge::CacheContext::current(lock)
    }

    #[jsg_property(lazy_instance)]
    pub fn get_version<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        match &self.version {
            Some(version) => version.as_local(lock),
            None => lock.undefined(),
        }
    }

    /// The Cloudflare Access information of the current request, if any.
    #[jsg_property(lazy_instance)]
    pub fn get_access(
        &self,
        lock: &mut jsg::Lock<'_>,
    ) -> jsg::Result<Option<jsg_cpp::Ref<bridge::AccessContext>>> {
        bridge::AccessContext::current(lock)
    }

    /// Routes `<host>:<port>` to `fetcher` for the request's outgoing connections, and returns
    /// `<host>:<port>`. The host is chosen on the first call for a `fetcher`, whose port then
    /// sticks.
    #[jsg_method]
    pub fn map_virtual_host(
        &self,
        lock: &mut jsg::Lock<'_>,
        fetcher: jsg_cpp::Ref<bridge::Fetcher>,
        port: u16,
    ) -> jsg::Result<String> {
        fetcher.get(lock).map_virtual_host(lock, port)
    }

    /// `ctx.tracing`, the user tracing API.
    #[jsg_property(lazy_instance)]
    pub fn get_tracing(
        &self,
        lock: &mut jsg::Lock<'_>,
    ) -> jsg::Result<jsg_cpp::Ref<bridge::Tracing>> {
        bridge::Tracing::new(lock)
    }

    /// Aborts the request with `reason`, and terminates execution.
    #[jsg_method]
    pub fn abort<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        reason: Option<v8::Local<'s, v8::Value>>,
    ) -> jsg::Result<()> {
        Err(IoContext::current(lock)?.abort(lock, reason))
    }
}

impl Resource for ExecutionContext {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::wait_until_member());
        registry.add(Self::pass_through_on_exception_member());
        if flags.get_enable_ctx_exports() {
            registry.add(Self::get_exports_member(PropertyPlacement::LazyInstance));
        }
        registry.add(Self::get_props_member(PropertyPlacement::LazyInstance));
        if flags.get_allow_irrevocable_stub_storage() {
            registry.add(Self::restore_member());
        }
        registry.add(Self::get_cache_member(PropertyPlacement::LazyInstance));
        if flags.get_enable_version_api() {
            registry.add(Self::get_version_member(PropertyPlacement::LazyInstance));
        }
        registry.add(Self::get_access_member(PropertyPlacement::LazyInstance));
        registry.add(Self::map_virtual_host_member());
        registry.add(Self::get_tracing_member(PropertyPlacement::LazyInstance));
        registry.add(Self::abort_member());
    }
}
