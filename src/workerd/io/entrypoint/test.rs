// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The test event, which C++ `WorkerEntrypoint::test()` hands to Rust: `workerd test` and other
//! test runners deliver it to each entrypoint's `test()` handler.
//!
//! The handler is called through the test-api crate, with a new `TestController`. The event has
//! only rudimentary tracing: it is reported to the tracer as a custom event, enough for its logs
//! and spans to reach tail workers, and has no return event.

use std::future::Future;

use cxx::KjException;
use io_context::IoContext;
use outcome_capnp::EventOutcome;

use crate::Invocation;

/// Starts the test event of `invocation`: reports it to the tracer, delivers it, calls the handler
/// in a waitUntil task and starts finishing the request. Returns the future for whether the test
/// passed: whether the request's waitUntil tasks, which include the handler's promise, all
/// succeeded. Dropping it cancels the request.
///
/// # Errors
/// Fails with the exception that one of the steps before the wait fails with.
pub fn start(
    mut invocation: Invocation,
) -> Result<impl Future<Output = Result<bool, KjException>> + 'static, KjException> {
    invocation.set_custom_event_info()?;
    let delivered = invocation.deliver("test", io_context::delivered_location!())?;
    // One waitUntil task, under the span "WorkerEntrypoint::test() run", runs the handler and waits
    // for its promise. Its C++ prelude finds the handler's test() function, and fails with the
    // exception that getTestHandler() throws under Service Workers syntax or without one.
    let running = delivered.run_test_in_wait_until(|lock, handler| {
        let promise = test_api::run(
            lock,
            handler.test,
            handler.handler,
            handler.env,
            handler.ctx,
        )?;
        Ok(IoContext::current(lock)?.await_js(lock, promise, |_lock, _value| Ok(()))?)
    })?;
    // The span covers the whole wait, from finishScheduled() until the request's waitUntil tasks
    // are done, so it is open across the await, on a track of its own. It ends when the wait does,
    // or when the future is dropped.
    let wait_span = perfetto::AsyncSlice::begin(c"WorkerEntrypoint::test() waitForFinished()");
    let finished = running.finish_scheduled();
    // There is no return event: test events have only the tracing that wd-tests need.
    Ok(async move {
        let _wait_span = wait_span;
        Ok(finished.await?.outcome == EventOutcome::Ok)
    })
}
