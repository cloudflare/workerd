// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the fibers of `fiber.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and main_stack_call_run() opens a scope \
              C++ has set up"
)]

use std::pin::Pin;

use jsg::v8;
use kj_rs::KjOwn;

pub use crate::fiber::FiberTask;
pub use crate::fiber::MainStackCall;

#[cxx::bridge(namespace = "workerd::api::html_rewriter")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    /// How `main_stack_call_run()` ended.
    #[repr(u8)]
    enum MainStackOutcome {
        /// The handler was called, or calling it failed and the exception was kept. Its settler
        /// has been settled, or will be when its promise settles.
        Dispatched,
        /// Execution is terminating, and the settler was dropped.
        Terminated,
        /// The call panicked. `jsg::catch_panic()` logged the panic, threw an internal error and
        /// requested termination.
        Panicked,
    }

    /// How `run_on_main_stack()` ended.
    #[repr(u8)]
    enum WaitOutcome {
        /// The call ran and settled its settler.
        Continue,
        /// The wait failed or was canceled, now or before; C++ rethrows why once lol-html
        /// returns.
        Stopped,
    }

    unsafe extern "C++" {
        include!("workerd/api/html-rewriter/fiber.h");

        type FiberWait;
        type Settler;

        fn start_fiber(task: Box<FiberTask>) -> Result<()>;
        fn run_on_main_stack(wait: Pin<&mut FiberWait>, call: &mut MainStackCall) -> WaitOutcome;
        fn take_wait_error(wait: Pin<&mut FiberWait>) -> Result<()>;
        fn settle(settler: KjOwn<Settler>);
    }

    impl KjOwn<Settler> {}

    extern "Rust" {
        type FiberTask;
        type MainStackCall;

        /// The constructor function of `HtmlRewriterUtils`, whose static methods are the
        /// crate's entry points. C++ calls this with the isolate locked, a context entered and a
        /// `HandleScope` open.
        unsafe fn new_utils(isolate: *mut Isolate) -> MaybeLocal;

        fn fiber_task_run(task: &mut FiberTask, wait: Pin<&mut FiberWait>);
        unsafe fn main_stack_call_run(
            isolate: usize,
            call: &mut MainStackCall,
            settler: KjOwn<Settler>,
        ) -> MainStackOutcome;
    }
}

// SAFETY: a `Settler` holds a cross-thread fulfiller (see `fiber.h`), which any thread may fulfill
// or destroy.
unsafe impl Send for bridge::Settler {}

/// The constructor function of `HtmlRewriterUtils`, or an empty handle with an exception pending.
///
/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn new_utils(isolate: *mut bridge::Isolate) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            jsg::resource::function_template_of::<crate::utils::HtmlRewriterUtils>(lock)
                .map(v8::Local::<v8::Value>::from)
        })
    }
}

/// Runs `task` on the fiber C++ started for it.
fn fiber_task_run(task: &mut FiberTask, wait: Pin<&mut bridge::FiberWait>) {
    task.run(wait);
}

/// Runs `call` on the main stack, from `run_on_main_stack()` in the task's `IoContext`, with the
/// isolate locked, a context entered and a `HandleScope` open.
unsafe fn main_stack_call_run(
    isolate: usize,
    call: &mut MainStackCall,
    settler: KjOwn<bridge::Settler>,
) -> bridge::MainStackOutcome {
    let scope = std::pin::pin!(
        // SAFETY: C++ calls this with the isolate locked, a context entered and a HandleScope
        // open, which outlive the scope.
        unsafe { jsg::CallbackScope::new(isolate as *mut v8::ffi::Isolate) }
    );
    jsg::catch_panic(scope, |lock| call.run(lock, settler))
        .unwrap_or(bridge::MainStackOutcome::Panicked)
}
