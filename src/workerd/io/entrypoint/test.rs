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
use jsg::v8;
use outcome_capnp::EventOutcome;

use crate::Invocation;
use crate::RunError;

/// Starts the test event of `invocation`, as `WorkerEntrypoint::test()` did in C++: reports it to
/// the tracer, delivers it, calls the handler in a waitUntil task and starts finishing the
/// request. Returns the future for whether the test passed: whether the request's waitUntil tasks,
/// which include the handler's promise, all succeeded. Dropping it cancels the request.
///
/// # Errors
/// Fails with the exception that one of the steps before the wait fails with.
pub fn start(
    mut invocation: Invocation,
) -> Result<impl Future<Output = Result<bool, KjException>> + 'static, KjException> {
    invocation.set_custom_event_info()?;
    let delivered = invocation.deliver(c"test")?;
    // One waitUntil task runs the handler and waits for its promise.
    let running =
        delivered.run_in_wait_until(c"WorkerEntrypoint::test() run", |lock, handler| {
            // TODO(someday): For Service Workers syntax, do we want addEventListener("test")? Not
            //   supporting it for now.
            if handler.handler.is_undefined() {
                return Err(js_error(
                    "Tests are not currently supported with Service Workers syntax.",
                ));
            }
            let no_test = || js_error("Entrypoint does not export a test() function.");
            let object =
                v8::Local::<v8::Object>::try_from(handler.handler).map_err(|_| no_test())?;
            let test = object.get(lock, "test")?;
            let test = v8::Local::<v8::Function>::try_from(test).map_err(|_| no_test())?;
            let promise = test_api::run(lock, test, handler.handler, handler.env, handler.ctx)?;
            Ok(IoContext::current(lock)?.await_js(lock, promise, |_lock, _value| Ok(()))?)
        })?;
    let finished = running.finish_scheduled(c"WorkerEntrypoint::test() waitForFinished()");
    Ok(async move { Ok(finished.await?.outcome == EventOutcome::Ok) })
}

/// A JavaScript `Error` with `message`, as C++ `JSG_REQUIRE(..., Error, message)` throws it: a KJ
/// exception whose description names the error. The run fails with it unchanged, so that the
/// waitUntil task logs it as it was thrown, and not also as an exception thrown by JavaScript, as
/// it would be had it been thrown into JavaScript.
fn js_error(message: &str) -> RunError {
    RunError::Kj(kj::failed!("jsg.Error: {message}").into())
}
