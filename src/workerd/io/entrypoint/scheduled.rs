// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The scheduled event (a Cron Trigger), which C++ `WorkerEntrypoint::runScheduled()` hands to
//! Rust.
//!
//! The handler is started in C++, by `ServiceWorkerGlobalScope::startScheduled()`, which creates
//! the `ScheduledController` and `ScheduledEvent` it receives. Rust runs the event around it.

use std::future::Future;

use cxx::KjException;
use kj_rs::KjDate;

use crate::Invocation;

/// Starts the scheduled event of `invocation`, for `scheduled_time` and the cron pattern `cron`, as
/// `WorkerEntrypoint::runScheduled()` does in C++: reports it to the tracer, delivers it, starts
/// the handler in a waitUntil task and starts finishing the request. Returns the future for the
/// result, which waits for the request's waitUntil tasks; dropping it cancels the request.
///
/// # Errors
/// Fails with the exception that one of the steps before the wait fails with.
pub fn start(
    mut invocation: Invocation,
    scheduled_time: KjDate,
    cron: &[u8],
) -> Result<impl Future<Output = Result<worker::ScheduledResult, KjException>> + 'static, KjException>
{
    invocation.set_scheduled_event_info(scheduled_time, cron)?;
    let delivered = invocation.deliver(c"scheduled")?;
    let running = delivered.start_scheduled_in_wait_until(scheduled_time, cron)?;
    Ok(running.finish_scheduled(c"WorkerEntrypoint::runScheduled() waitForFinished()"))
}
