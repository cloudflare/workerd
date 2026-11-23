// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ dispatches the bench event to a Worker's `bench()` handler, and
//! the C++ it calls.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and bench_run() takes C++'s raw isolate \
              and handles"
)]

use jsg::v8;

use crate::BenchController;
use crate::runner;

#[cxx::bridge(namespace = "workerd::api")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/util/thread-scopes.h");

        /// Whether the process runs `workerd bench`, the only command that dispatches the bench
        /// event. Not `is_bench_mode`, which workerd-server's bridge declares: cxx names each
        /// shim after the function and its namespace, so the two would collide at link time.
        #[cxx_name = "isBenchMode"]
        fn in_bench_mode() -> bool;
    }

    extern "Rust" {
        /// Runs the `bench()` handler of `handler`, a Worker's exported handler object, with
        /// `env` and `ctx` (undefined if it has none), then the cases it registers. `params` is
        /// an encoded `BenchParams` message, in words. Returns a promise for the group: an
        /// `ArrayBuffer` that holds its `BenchReport.Group`, without its name, as an encoded
        /// message, which rejects if the runner itself fails. Throws if `handler` has no
        /// `bench()`.
        unsafe fn bench_run(
            isolate: *mut Isolate,
            handler: Local,
            env: Local,
            ctx: Local,
            params: &[u64],
        ) -> MaybeLocal;
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open, in which
/// `handler`, `env` and `ctx` are handles.
unsafe fn bench_run(
    isolate: *mut bridge::Isolate,
    handler: bridge::Local,
    env: bridge::Local,
    ctx: bridge::Local,
    params: &[u64],
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller, who also passes `handler`, `env` and `ctx` in the scope
    // that the closure's lock stands for.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            // The handles are in the caller's scope, which is the lock's.
            let (handler, env, ctx) = (
                v8::Local::<v8::Value>::from_ffi(handler),
                v8::Local::<v8::Value>::from_ffi(env),
                v8::Local::<v8::Value>::from_ffi(ctx),
            );
            // Fails closed anywhere but `workerd bench`.
            if !bridge::in_bench_mode() {
                return Err(jsg::Error::new_internal_error(
                    "the bench event is only supported by `workerd bench`",
                ));
            }
            let handler_object = v8::Local::<v8::Object>::try_from(handler).map_err(|_| {
                jsg::Error::new_error("Entrypoint does not export a bench() function.")
            })?;
            let bench = handler_object.get(lock, "bench")?;
            let bench = v8::Local::<v8::Function>::try_from(bench).map_err(|_| {
                jsg::Error::new_error("Entrypoint does not export a bench() function.")
            })?;
            let controller = BenchController::read(params)
                .map_err(|err| jsg::Error::new_internal_error(err.to_string()))?;
            let controller = jsg::Rc::new(lock, controller);
            runner::start(lock, &controller, bench, handler, env, ctx)
        })
    }
}
