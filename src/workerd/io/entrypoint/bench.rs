// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bench event, which C++ `WorkerEntrypoint::bench()` hands to Rust: `workerd bench` delivers
//! it to each entrypoint's `bench()` handler.
//!
//! The handler is started in C++, through `Worker::Api::runBench()`, which only `workerd bench`
//! implements, with the bench-api crate: it runs the cases the handler registers and resolves with
//! their group. Rust waits for the group and drains the request. The event is reported to the
//! tracer as a custom event and has no return event, as the test event.

use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;

use cxx::KjException;
use io_context::IoContext;
use jsg::v8;

use crate::Invocation;

/// Starts the bench event of `invocation`, with `params`, an encoded `BenchParams` message in
/// words: reports it to the tracer, delivers it and starts the handler's `bench()` in
/// `IoContext::run()`. Returns the future that waits for the cases it registers to run and
/// drains the request, and resolves with the group's `BenchReport.Group`, without its name, as an
/// encoded message in words. Dropping it cancels the run and drains the request.
///
/// # Errors
/// Fails with the exception that reporting or delivering the event fails with.
pub fn start(
    mut invocation: Invocation,
    params: &[u64],
) -> Result<impl Future<Output = Result<Vec<u64>, KjException>> + 'static, KjException> {
    invocation.set_custom_event_info()?;
    let delivered = invocation.deliver("bench", io_context::delivered_location!())?;
    // Where the run leaves the group, since the run's future resolves with nothing.
    let group = Rc::new(RefCell::new(None));
    let slot = Rc::clone(&group);
    // Unlike the test event, the event awaits the handler directly rather than through
    // finishScheduled(), since the report needs the handler's result.
    let ran = delivered.run_bench(params, move |lock, promise| {
        let encoded = IoContext::current(lock)?.await_js(lock, promise, |lock, value| {
            let buffer = v8::Local::<v8::ArrayBuffer>::try_from(value).map_err(|_| {
                jsg::Error::new_internal_error("the bench runner resolved with no ArrayBuffer")
            })?;
            to_words(buffer.as_slice(lock))
        })?;
        Ok(async move {
            *slot.borrow_mut() = Some(encoded.await?);
            Ok(())
        })
    });
    Ok(async move {
        ran.await?;
        group
            .take()
            .ok_or_else(|| kj::failed!("the bench run succeeded without a group").into())
    })
}

/// `bytes`, an encoded message, as words, so that C++ can read it without copying it again.
fn to_words(bytes: &[u8]) -> jsg::Result<Vec<u64>> {
    let (words, rest) = bytes.as_chunks::<8>();
    if !rest.is_empty() {
        return Err(jsg::Error::new_internal_error(format!(
            "the bench runner's group has {} bytes, not a whole number of words",
            bytes.len()
        )));
    }
    Ok(words.iter().map(|word| u64::from_ne_bytes(*word)).collect())
}
