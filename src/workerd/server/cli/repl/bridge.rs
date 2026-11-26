// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI between `workerd repl` and its C++ half (repl/repl.h): the session's I/O and V8, the
//! workers input is evaluated in, the rendering of results, and the factories of the configs'
//! servers.

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue, and calls its shim that takes \
              C++'s raw isolate"
)]

#[cxx::bridge(namespace = "workerd::server::repl")]
pub mod ffi {
    /// The process's settings, which its first session sets up and every later one must ask for.
    struct ReplProcessOptions {
        /// `--autogate`: the process's autogates, by kebab-case name.
        autogates: Vec<String>,
        /// `--v8-flag`: the process's V8 flags.
        v8_flags: Vec<String>,
    }

    /// What a worker input is evaluated in is compiled with.
    struct ReplWorkerOptions {
        compat_date: String,
        /// Every flag, those the REPL adds included (environment.rs).
        compat_flags: Vec<String>,
        /// `--experimental`, for the compatibility flags and the loaded configs.
        experimental: bool,
    }

    #[namespace = "kj_rs_tokio"]
    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        type TokioAsyncIoContext = kj_rs_tokio::TokioAsyncIoContext;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
    }

    #[namespace = "workerd::io_context"]
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IncomingRequest = io_context::IncomingRequest;
    }

    #[namespace = "workerd::api::channel_interop"]
    unsafe extern "C++" {
        include!("workerd/api/channel-interop/ffi.h");

        /// A C++ channel, as `Fetcher::from_channel()` takes one.
        #[cxx_name = "SubrequestChannel"]
        type IoSubrequestChannel = channel_interop::SubrequestChannel;
    }

    // The server crate's bridge declares the factory and the channel.
    #[namespace = "workerd::server"]
    unsafe extern "C++" {
        include!("workerd/server/server/bridge.rs.h");

        type WorkerFactory = workerd_server::bridge::ffi::WorkerFactory;
    }

    #[namespace = "workerd::server"]
    extern "Rust" {
        type SubrequestChannel = workerd_server::channels::SubrequestChannel;
    }

    unsafe extern "C++" {
        include!("workerd/server/repl/repl.h");

        /// The I/O of a session's workers and of the configs they load: the event loop's timer,
        /// network and filesystem, and the process's V8.
        type ReplSession;

        /// A session on the event loop `event_loop`, which it must not outlive.
        fn repl_session_new(
            event_loop: Pin<&mut TokioAsyncIoContext>,
            options: &ReplProcessOptions,
        ) -> Result<KjOwn<ReplSession>>;

        /// The session's resources, which the session outlives.
        type ReplResources;
        fn repl_session_resources(session: Pin<&mut ReplSession>) -> KjRc<ReplResources>;

        /// A worker that input is evaluated in, and that never receives an event.
        type ReplWorker;

        /// A worker on the session's resources. An error, naming the problems, if the
        /// compatibility settings are not valid.
        fn repl_worker_new(
            resources: &ReplResources,
            options: &ReplWorkerOptions,
        ) -> Result<KjOwn<ReplWorker>>;

        /// A new request on the worker, not yet delivered.
        fn repl_worker_new_request(worker: Pin<&mut ReplWorker>) -> KjOwn<IncomingRequest>;

        /// The name input is compiled under: the URL of the worker's main module.
        fn repl_worker_input_name(worker: &ReplWorker) -> &str;

        /// What the servers of the configs loaded in the worker's environment are built from.
        fn repl_worker_factory_source(worker: Pin<&mut ReplWorker>) -> KjRc<ReplFactorySource>;

        /// Renders `value` as the runtime's `inspect()` does, into `result`. Runs in C++
        /// `jsg_cpp::callCpp()`: false, with the exception pending, if it threw.
        ///
        /// # Safety
        /// The isolate is locked by this thread with a context entered and a `HandleScope`
        /// open, and `value` is a handle in that scope.
        unsafe fn repl_inspect(
            isolate: *mut Isolate,
            value: Local,
            colors: bool,
            result: &mut String,
        ) -> bool;

        /// A loaded server's channel as C++ holds one. Interim, until the server hands out
        /// C++ channels itself.
        fn repl_channel_into_kj(channel: Box<SubrequestChannel>) -> KjRc<IoSubrequestChannel>;

        /// What the servers of an environment's configs are built from: the session's process, timer,
        /// network and filesystem, and the environment's header table and `--experimental`.
        type ReplFactorySource;

        /// A worker factory for `config`, an encoded `Config` message.
        fn repl_new_factory(
            source: &ReplFactorySource,
            config: Vec<u64>,
        ) -> Result<KjOwn<WorkerFactory>>;
    }
}

/// `value` as the runtime's `inspect()` renders it, with ANSI colors if `colors`.
///
/// # Errors
/// Fails with what inspecting it throws, such as for a throwing getter or a revoked proxy.
pub fn inspect<'s>(
    lock: &mut jsg::Lock<'s>,
    value: jsg::v8::Local<'s, jsg::v8::Value>,
    colors: bool,
) -> jsg::Result<String> {
    let mut text = String::new();
    jsg_cpp::call_cpp(lock, |isolate| {
        // SAFETY: `call_cpp` calls the shim with the lock's isolate, and `value` is a handle in
        // the lock's scope.
        unsafe { ffi::repl_inspect(isolate, value.into_ffi(), colors, &mut text) }
    })?;
    Ok(text)
}
