// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, the shims take C++'s raw isolate, and the \
              stub handle is sent between threads; see the SAFETY comments"
)]

//! The bridge to `ffi.h`, and the Rust API over it.

use std::future::Future;

use channel_interop::DurableObjectClass;
use channel_interop::Fetcher;
use channel_interop::SubrequestChannel;
use frankenvalue::Frankenvalue;
use io_context::IoContext;
use io_context::ReentryCallback;
use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::KjRc;

// Every function that takes an isolate takes the isolate of the caller's lock, locked by this
// thread with a context entered and a `HandleScope` open.
#[cxx::bridge(namespace = "workerd::api::worker_loader_interop")]
pub mod bridge {
    /// The kind of a module, as C++ `WorkerSource::ModuleContent` distinguishes them.
    #[repr(u8)]
    enum ModuleKind {
        Es,
        CommonJs,
        Text,
        Data,
        Json,
        Python,
        Wasm,
    }

    /// C++ `CompatibilityDateValidation` (`workerd/io/compatibility-date.h`): the compatibility
    /// dates a Worker Loader accepts.
    #[namespace = "workerd"]
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(i32)]
    enum CompatibilityDateValidation {
        CODE_VERSION,
        CURRENT_DATE_FOR_CLOUDFLARE,
        FUTURE_FOR_TEST,
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/compatibility-date.h");
        type CompatibilityDateValidation;
    }

    /// C++ `kj::Maybe<ResourceLimits>`: `present` says whether there are limits at all.
    struct Limits {
        present: bool,
        has_cpu_ms: bool,
        cpu_ms: u32,
        has_sub_requests: bool,
        sub_requests: u32,
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type CompiledWasmModule = jsg::v8::ffi::CompiledWasmModule;
    }

    #[namespace = "workerd::rust::jsg_cpp"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg-cpp/jsg-cpp.h");
        type CppRef = jsg_cpp::CppRef;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/frankenvalue/ffi.h");
        type Frankenvalue = frankenvalue::Frankenvalue;
    }

    #[namespace = "workerd::api::channel_interop"]
    unsafe extern "C++" {
        include!("workerd/api/channel-interop/ffi.h");
        type SubrequestChannel = channel_interop::SubrequestChannel;
    }

    unsafe extern "C++" {
        include!("workerd/api/worker-loader-interop/ffi.h");

        /// A C++ `DynamicWorkerSource` under construction.
        type SourceBuilder;

        /// A `WorkerStub`'s C++ `IoOwn<WorkerStubChannel>`.
        type WorkerStubHandle;

        fn source_builder_new(main_module: String) -> KjOwn<SourceBuilder>;
        fn source_builder_add_text(
            builder: Pin<&mut SourceBuilder>,
            kind: u8,
            name: String,
            body: String,
        ) -> Result<()>;
        fn source_builder_add_bytes(
            builder: Pin<&mut SourceBuilder>,
            kind: u8,
            name: String,
            body: Vec<u8>,
        ) -> Result<()>;
        fn source_builder_set_limits(builder: Pin<&mut SourceBuilder>, limits: &Limits);
        fn source_builder_set_env(builder: Pin<&mut SourceBuilder>, env: KjOwn<Frankenvalue>);
        fn source_builder_set_global_outbound(
            builder: Pin<&mut SourceBuilder>,
            channel: KjRc<SubrequestChannel>,
        );
        fn source_builder_add_tail(
            builder: Pin<&mut SourceBuilder>,
            channel: KjRc<SubrequestChannel>,
        );
        fn source_builder_add_streaming_tail(
            builder: Pin<&mut SourceBuilder>,
            channel: KjRc<SubrequestChannel>,
        );

        // Per-method shims. Each returns false with an exception pending if the C++ threw.
        unsafe fn source_builder_add_compiled_wasm(
            isolate: *mut Isolate,
            builder: Pin<&mut SourceBuilder>,
            name: String,
            module: KjOwn<CompiledWasmModule>,
        ) -> bool;
        unsafe fn source_builder_compile_compat_flags(
            isolate: *mut Isolate,
            builder: Pin<&mut SourceBuilder>,
            compat_date: &str,
            flags: &[String],
            allow_experimental: bool,
            date_validation: CompatibilityDateValidation,
        ) -> bool;
        unsafe fn source_builder_inherit_global_outbound(
            isolate: *mut Isolate,
            builder: Pin<&mut SourceBuilder>,
            io_context_id: u64,
        ) -> bool;
        unsafe fn worker_loader_load(
            isolate: *mut Isolate,
            io_context_id: u64,
            channel: u32,
            builder: KjOwn<SourceBuilder>,
            result: &mut KjMaybe<KjOwn<WorkerStubHandle>>,
        ) -> bool;
        unsafe fn worker_loader_get(
            isolate: *mut Isolate,
            io_context_id: u64,
            channel: u32,
            has_name: bool,
            name: &str,
            callback: Box<CodeCallback>,
            result: &mut KjMaybe<KjOwn<WorkerStubHandle>>,
        ) -> bool;
        #[expect(
            clippy::too_many_arguments,
            reason = "an out-parameter shim, as C++ declares it"
        )]
        unsafe fn worker_stub_get_entrypoint(
            isolate: *mut Isolate,
            stub: &WorkerStubHandle,
            io_context_id: u64,
            has_name: bool,
            name: &str,
            props: KjOwn<Frankenvalue>,
            limits: &Limits,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        #[expect(
            clippy::too_many_arguments,
            reason = "an out-parameter shim, as C++ declares it"
        )]
        unsafe fn worker_stub_get_actor_class(
            isolate: *mut Isolate,
            stub: &WorkerStubHandle,
            io_context_id: u64,
            has_name: bool,
            name: &str,
            props: KjOwn<Frankenvalue>,
            limits: &Limits,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
    }

    extern "Rust" {
        /// The callback of `WorkerLoader.get()`, which C++ calls, and drops, on the thread of the
        /// IoContext that made it.
        type CodeCallback;

        /// Calls `callback`: a promise for the source of the Worker it gets the code of.
        async fn code_callback_call(callback: &CodeCallback) -> Result<KjOwn<SourceBuilder>>;
    }

    impl KjOwn<SourceBuilder> {}
    impl KjOwn<WorkerStubHandle> {}
}

pub use bridge::CompatibilityDateValidation;
pub use bridge::Limits;
pub use bridge::ModuleKind;
pub use bridge::SourceBuilder;
pub use bridge::WorkerStubHandle;

// SAFETY: the handle holds a C++ `IoOwn`, which may be destroyed on any thread: off its
// IoContext's thread, it only queues the deletion for the IoContext (`DeleteQueue` in
// io-own.c++). It is dereferenced only by the shims, which run under the isolate lock and check
// that its IoContext is the current one, so on that IoContext's thread.
unsafe impl Send for WorkerStubHandle {}
// SAFETY: as for `Send`: `&WorkerStubHandle` reaches the channel only through the shims.
unsafe impl Sync for WorkerStubHandle {}

/// The callback of `WorkerLoader.get()`.
pub struct CodeCallback(pub ReentryCallback<KjOwn<SourceBuilder>>);

fn code_callback_call(
    callback: &CodeCallback,
) -> impl Future<Output = Result<KjOwn<SourceBuilder>, cxx::KjException>> + use<> {
    callback.0.call()
}

impl SourceBuilder {
    /// A new builder for a Worker whose main module is `main_module`.
    #[must_use]
    pub fn new(main_module: &str) -> KjOwn<Self> {
        bridge::source_builder_new(nul_terminated(main_module))
    }

    /// Adds a module whose body is text, as `kind`, which is not `Data` or `Wasm`. The builder
    /// takes `body`.
    ///
    /// # Errors
    /// Fails with an internal error for a kind whose body is bytes.
    pub fn add_text(
        self: std::pin::Pin<&mut Self>,
        kind: ModuleKind,
        name: &str,
        mut body: String,
    ) -> jsg::Result<()> {
        // The body moves to C++ with a NUL appended, rather than being copied.
        body.push('\0');
        Ok(bridge::source_builder_add_text(
            self,
            kind.repr,
            nul_terminated(name),
            body,
        )?)
    }

    /// Adds a `Data` or `Wasm` module of `body`, which the builder takes.
    ///
    /// # Errors
    /// Fails with an internal error for a kind whose body is text.
    pub fn add_bytes(
        self: std::pin::Pin<&mut Self>,
        kind: ModuleKind,
        name: &str,
        body: Vec<u8>,
    ) -> jsg::Result<()> {
        Ok(bridge::source_builder_add_bytes(
            self,
            kind.repr,
            nul_terminated(name),
            body,
        )?)
    }

    /// Adds a WASM module that shares the compiled code of `module`.
    ///
    /// # Errors
    /// A pending error if the C++ threw.
    pub fn add_compiled_wasm(
        self: std::pin::Pin<&mut Self>,
        lock: &mut jsg::Lock<'_>,
        name: &str,
        module: v8::CompiledWasmModule,
    ) -> jsg::Result<()> {
        let name = nul_terminated(name);
        let module = module.into_ffi();
        jsg_cpp::call_cpp(lock, |isolate| {
            // SAFETY: `call_cpp` calls the shim with the lock's isolate.
            unsafe { bridge::source_builder_add_compiled_wasm(isolate, self, name, module) }
        })
    }

    /// Compiles the code's compatibility date and flags with C++ `compileCompatibilityFlags()`,
    /// as the C++ Worker Loader does.
    ///
    /// # Errors
    /// A pending error with the compiler's first error, as C++ Worker Loader throws it.
    pub fn compile_compat_flags(
        self: std::pin::Pin<&mut Self>,
        lock: &mut jsg::Lock<'_>,
        compat_date: &str,
        flags: &[String],
        allow_experimental: bool,
        date_validation: CompatibilityDateValidation,
    ) -> jsg::Result<()> {
        jsg_cpp::call_cpp(lock, |isolate| {
            // SAFETY: `call_cpp` calls the shim with the lock's isolate.
            unsafe {
                bridge::source_builder_compile_compat_flags(
                    isolate,
                    self,
                    compat_date,
                    flags,
                    allow_experimental,
                    date_validation,
                )
            }
        })
    }

    pub fn set_limits(self: std::pin::Pin<&mut Self>, limits: &Limits) {
        bridge::source_builder_set_limits(self, limits);
    }

    pub fn set_env(self: std::pin::Pin<&mut Self>, env: KjOwn<Frankenvalue>) {
        bridge::source_builder_set_env(self, env);
    }

    pub fn set_global_outbound(self: std::pin::Pin<&mut Self>, channel: KjRc<SubrequestChannel>) {
        bridge::source_builder_set_global_outbound(self, channel);
    }

    /// Has the Worker inherit the global outbound of the calling Worker.
    ///
    /// # Errors
    /// A pending error if the C++ threw, as unless `io_context` is current.
    pub fn inherit_global_outbound<'s>(
        self: std::pin::Pin<&mut Self>,
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
    ) -> jsg::Result<()> {
        let id = io_context.as_ffi();
        jsg_cpp::call_cpp(lock, |isolate| {
            // SAFETY: `call_cpp` calls the shim with the lock's isolate.
            unsafe { bridge::source_builder_inherit_global_outbound(isolate, self, id) }
        })
    }

    pub fn add_tail(self: std::pin::Pin<&mut Self>, channel: KjRc<SubrequestChannel>) {
        bridge::source_builder_add_tail(self, channel);
    }

    pub fn add_streaming_tail(self: std::pin::Pin<&mut Self>, channel: KjRc<SubrequestChannel>) {
        bridge::source_builder_add_streaming_tail(self, channel);
    }
}

/// Loads the Worker that `builder` describes on the loader channel `channel`, as `load()` does.
///
/// # Errors
/// A pending error if the C++ threw.
pub fn load<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    channel: u32,
    builder: KjOwn<SourceBuilder>,
) -> jsg::Result<KjOwn<WorkerStubHandle>> {
    let id = io_context.as_ffi();
    let mut slot = KjMaybe::None;
    jsg_cpp::call_cpp(lock, |isolate| {
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        unsafe { bridge::worker_loader_load(isolate, id, channel, builder, &mut slot) }
    })?;
    Ok(Option::from(slot).unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
}

/// Loads, or finds, the Worker named `name` on the loader channel `channel`, whose source
/// `callback` gets, as `get()` does.
///
/// # Errors
/// A pending error if the C++ threw.
pub fn get<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    channel: u32,
    name: Option<&str>,
    callback: CodeCallback,
) -> jsg::Result<KjOwn<WorkerStubHandle>> {
    let id = io_context.as_ffi();
    let mut slot = KjMaybe::None;
    let callback = Box::new(callback);
    jsg_cpp::call_cpp(lock, |isolate| {
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        unsafe {
            bridge::worker_loader_get(
                isolate,
                id,
                channel,
                name.is_some(),
                name.unwrap_or_default(),
                callback,
                &mut slot,
            )
        }
    })?;
    Ok(Option::from(slot).unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
}

impl WorkerStubHandle {
    /// A `Fetcher` for the entrypoint `name` (`None` for the default one) of the stub's Worker.
    ///
    /// # Errors
    /// A pending error if the C++ threw, as unless `io_context` is the stub's.
    pub fn entrypoint<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
        name: Option<&str>,
        props: KjOwn<Frankenvalue>,
        limits: &Limits,
    ) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
        let id = io_context.as_ffi();
        // SAFETY: the shim instantiates callCppForRef with api::Fetcher, and `call_cpp_for_ref`
        // calls it with the lock's isolate.
        let fetcher = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::worker_stub_get_entrypoint(
                    isolate,
                    self,
                    id,
                    name.is_some(),
                    name.unwrap_or_default(),
                    props,
                    limits,
                    result,
                )
            })
        }?;
        Ok(fetcher.unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }

    /// A `DurableObjectClass` for the class `name` (`None` for the default one) of the stub's
    /// Worker.
    ///
    /// # Errors
    /// As for [`entrypoint`](Self::entrypoint).
    pub fn actor_class<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
        name: Option<&str>,
        props: KjOwn<Frankenvalue>,
        limits: &Limits,
    ) -> jsg::Result<jsg_cpp::Ref<DurableObjectClass>> {
        let id = io_context.as_ffi();
        // SAFETY: the shim instantiates callCppForRef with api::DurableObjectClass, and
        // `call_cpp_for_ref` calls it with the lock's isolate.
        let class = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::worker_stub_get_actor_class(
                    isolate,
                    self,
                    id,
                    name.is_some(),
                    name.unwrap_or_default(),
                    props,
                    limits,
                    result,
                )
            })
        }?;
        Ok(class.unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }
}

/// `text` followed by a NUL, so that C++ can view it as a `kj::StringPtr` without copying it.
fn nul_terminated(text: &str) -> String {
    let mut owned = String::with_capacity(text.len() + 1);
    owned.push_str(text);
    owned.push('\0');
    owned
}
