// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to `ffi.c++`, which sets V8's WebAssembly callbacks to the crate's and wraps the
//! C++ the crate needs: V8's `WasmStreaming`, the isolate's foreground task signal, and
//! `IoChannelFactory::submitWasmModule()`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and its functions take C++'s raw isolate \
              and handles"
)]

use jsg::v8;

use crate::dynamic::ModuleBytes;

#[cxx::bridge(namespace = "workerd::api::wasm")]
#[expect(
    clippy::allow_attributes,
    clippy::allow_attributes_without_reason,
    reason = "cxx expands an `allow` without a reason"
)]
#[expect(
    clippy::needless_lifetimes,
    reason = "cxx needs the lifetime of a returned reference spelled out"
)]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    unsafe extern "C++" {
        include!("workerd/api/wasm-api/ffi.h");

        /// One streaming compilation: V8's `WasmStreaming`. Dropped without `finish()` or
        /// `abort()`, it aborts from the isolate's next foreground task.
        type WasmStreaming;
        /// Must be called with the isolate lock, before `finish()` or `abort()`.
        #[cxx_name = "onBytesReceived"]
        fn on_bytes_received(self: Pin<&mut WasmStreaming>, bytes: &[u8]);
        /// Must be called with the isolate lock, before `finish()` or `abort()`.
        #[cxx_name = "setUrl"]
        fn set_url(self: Pin<&mut WasmStreaming>, url: &str);
        /// Must be called with the isolate lock, once, unless `abort()` is.
        fn finish(self: Pin<&mut WasmStreaming>);
        /// # Safety
        /// The caller holds the isolate lock, `exception` is a handle in an open `HandleScope`,
        /// and neither this nor `finish()` was called before.
        unsafe fn abort(self: Pin<&mut WasmStreaming>, exception: Local);

        /// The isolate's foreground task signal, which its runners notify whenever V8 posts a
        /// task.
        type ForegroundTaskSignal;
        /// # Safety
        /// The caller holds the isolate lock.
        unsafe fn foreground_task_signal(isolate: *mut Isolate) -> KjArc<ForegroundTaskSignal>;
        fn foreground_task_signal_epoch(signal: &ForegroundTaskSignal) -> u64;
        fn foreground_task_signal_notify(signal: &ForegroundTaskSignal);
        /// Resolves once the epoch is past `seen`. Must be awaited on a thread with a KJ event
        /// loop.
        async fn foreground_task_signal_wait_after(signal: &ForegroundTaskSignal, seen: u64);

        /// The promise `IoChannelFactory::submitWasmModule()` returned for a module.
        type WasmSubmission;
        /// Hands `bytes` to the current request's embedder, which fails this to refuse them.
        /// Must be called with the isolate lock in a request.
        fn submit_wasm_module(bytes: Box<ModuleBytes>) -> Result<KjOwn<WasmSubmission>>;
        /// Resolves once the embedder accepts the module, and fails if it rejects it.
        async fn wasm_submission_wait(submission: KjOwn<WasmSubmission>) -> Result<()>;
        /// Condemns the isolate. Must be called with the isolate lock in a request.
        fn abort_isolate_for_refused_module() -> Result<()>;
    }

    extern "Rust" {
        type ModuleBytes;
        /// # Safety
        /// The slice is valid until the bytes are dropped.
        unsafe fn as_slice<'a>(self: &'a ModuleBytes) -> &'a [u8];

        /// # Safety
        /// V8 calls this as the isolate's `WasmStreamingCallback`, with the isolate lock and a
        /// `HandleScope` open.
        unsafe fn wasm_streaming_callback(
            isolate: *mut Isolate,
            value: Local,
            streaming: KjOwn<WasmStreaming>,
        );

        /// # Safety
        /// V8 calls this as the isolate's `WasmModuleCallback`, with the isolate lock and a
        /// `HandleScope` open.
        unsafe fn wasm_module_callback(isolate: *mut Isolate, source: Local) -> bool;

        /// # Safety
        /// V8 calls this for each compilation it starts, with the isolate lock and a
        /// `HandleScope` open.
        unsafe fn allows_code_generation(isolate: *mut Isolate) -> bool;

        /// The function through which the WebAssembly shim calls V8's asynchronous compile
        /// functions; see `compile.rs`.
        ///
        /// # Safety
        /// The caller holds the isolate lock with a context entered and a `HandleScope` open.
        unsafe fn new_compile_function(isolate: *mut Isolate) -> MaybeLocal;
    }
}

// SAFETY: a `const jsg::ForegroundTaskSignal` is atomically refcounted, and its methods are
// thread-safe: the epoch and the waiter count are atomics, and a mutex guards the waiters, which
// are cross-thread fulfillers.
unsafe impl Send for bridge::ForegroundTaskSignal {}
// SAFETY: as for `Send`.
unsafe impl Sync for bridge::ForegroundTaskSignal {}

// SAFETY: a `WasmSubmission` holds the `kj::Promise` the embedder returned, which Rust only moves
// into `wasm_submission_wait()`, whose future the IoContext's thread polls and drops.
unsafe impl Send for bridge::WasmSubmission {}

/// # Safety
/// See the bridge.
unsafe fn wasm_streaming_callback(
    isolate: *mut v8::ffi::Isolate,
    value: v8::ffi::Local,
    streaming: kj_rs::KjOwn<bridge::WasmStreaming>,
) {
    jsg::callback_scope!(unsafe let lock, isolate);
    // SAFETY: V8 passes a handle in the open `HandleScope`.
    let value = unsafe { v8::Local::from_ffi(value) };
    crate::streaming::start(lock, value, streaming);
}

/// # Safety
/// See the bridge.
unsafe fn wasm_module_callback(isolate: *mut v8::ffi::Isolate, source: v8::ffi::Local) -> bool {
    jsg::callback_scope!(unsafe let lock, isolate);
    // SAFETY: V8 passes a handle in the open `HandleScope`.
    let source = unsafe { v8::Local::from_ffi(source) };
    crate::dynamic::submit_from_constructor(lock, source)
}

/// # Safety
/// See the bridge.
unsafe fn allows_code_generation(isolate: *mut v8::ffi::Isolate) -> bool {
    jsg::callback_scope!(unsafe let lock, isolate);
    crate::dynamic::allows_dynamic_compilation(lock)
}

/// # Safety
/// See the bridge.
unsafe fn new_compile_function(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe { v8::run_ffi_callback(isolate, crate::compile::new_compile_function) }
}
