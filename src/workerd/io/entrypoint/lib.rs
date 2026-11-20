// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The events of a C++ `WorkerEntrypoint` that Rust delivers.
//!
//! `WorkerEntrypoint` (`src/workerd/io/worker-entrypoint.h`) delivers each event to its
//! Worker's exported handler. For an event that Rust implements, it hands Rust an
//! [`Invocation`]: the request it was made for, and what finds the handler. Rust then reports the
//! event to the tracer, delivers it, runs the handler, and lets the request finish, either in the
//! background or by waiting for it, as for a scheduled event.
//!
//! An invocation goes through three types, each of which is consumed by the step that leads to the
//! next, so that each step happens once and in order: an [`Invocation`] is delivered into a
//! [`Delivered`], whose handler is run into a [`Running`], which is finished. Every step calls C++
//! when it is called, not when a future it returns is first polled, so that the request is in the
//! state the step leaves it in as soon as the step returns, as in C++.
//!
//! See `AGENTS.md` for how the crate fits together and what is left to port.

mod ffi;
mod scheduled;

use std::ffi::CStr;
use std::future::Future;

use cxx::KjException;
use ffi::bridge;
use kj_rs::KjDate;
use kj_rs::KjOwn;

pub use crate::ffi::bridge::Invocation as InvocationFfi;

/// An invocation of a `WorkerEntrypoint` for an event, which it handed to Rust, not delivered yet.
/// Dropping it cancels the event before delivery, so that no JavaScript runs for it.
pub struct Invocation {
    invocation: KjOwn<bridge::Invocation>,
}

impl Invocation {
    #[must_use]
    pub fn new(invocation: KjOwn<bridge::Invocation>) -> Self {
        Self { invocation }
    }

    /// Tells the request's tracer, if it has one, that the event is a scheduled event for
    /// `scheduled_time` and the cron pattern `cron`.
    ///
    /// # Errors
    /// Fails with the exception the tracer throws.
    pub fn set_scheduled_event_info(
        &mut self,
        scheduled_time: KjDate,
        cron: &[u8],
    ) -> Result<(), KjException> {
        bridge::invocation_set_scheduled_event_info(self.invocation.pin_mut(), scheduled_time, cron)
    }

    /// Delivers the event: `IoContext::IncomingRequest::delivered()`. `event_type`, such as
    /// `c"scheduled"`, names the request's async trace resource.
    ///
    /// # Errors
    /// Fails with the exception that `delivered()` throws. The event is then dropped undelivered.
    pub fn deliver(mut self, event_type: &'static CStr) -> Result<Delivered, KjException> {
        ffi::delivered(self.invocation.pin_mut(), event_type)?;
        Ok(Delivered(Request(Some(self.invocation))))
    }
}

/// The request of a delivered event. Dropping it lets the request finish in the background, in
/// the `WorkerEntrypoint`'s waitUntil tasks, as `IoContext::IncomingRequest::drain()` does, unless
/// it was finished.
///
/// That includes dropping it because a step failed after delivery. In C++, an exception there
/// destroys the request without draining it; draining is the default here because it never cuts
/// short waitUntil tasks that the event has already started.
struct Request(Option<KjOwn<bridge::Invocation>>);

impl Request {
    fn invocation_mut(&mut self) -> &mut KjOwn<bridge::Invocation> {
        self.0
            .as_mut()
            .unwrap_or_else(|| unreachable!("taken only when finished or dropped"))
    }

    fn take(&mut self) -> KjOwn<bridge::Invocation> {
        self.0
            .take()
            .unwrap_or_else(|| unreachable!("taken only when finished or dropped"))
    }
}

impl Drop for Request {
    fn drop(&mut self) {
        if let Some(invocation) = self.0.take() {
            bridge::invocation_drain(invocation);
        }
    }
}

/// A delivered event, whose handler has not run. Dropping it lets the request finish in the
/// background.
pub struct Delivered(Request);

impl Delivered {
    /// Starts the event's scheduled handler with a `ScheduledEvent` for `scheduled_time` and
    /// `cron`, in a waitUntil task of the request, with the C++ function that
    /// `WorkerEntrypoint::runScheduled()` also calls, `entrypoint::startScheduledInWaitUntil()`:
    /// `ServiceWorkerGlobalScope::startScheduled()` calls the exported handler's `scheduled()`, or
    /// under Service Workers syntax dispatches the event to its `scheduled` listeners, and makes
    /// the handler's promise a waitUntil task. The task fails with the
    /// exception that the handler throws, or, if there is no handler, with the KJ exception that
    /// `startScheduled()` throws, and the request then is not retried.
    ///
    /// # Errors
    /// Fails with the exception that keeps the task from being added. The request then finishes
    /// in the background.
    pub fn start_scheduled_in_wait_until(
        mut self,
        scheduled_time: KjDate,
        cron: &[u8],
    ) -> Result<Running, KjException> {
        bridge::invocation_start_scheduled_in_wait_until(
            self.0.invocation_mut().pin_mut(),
            scheduled_time,
            cron,
        )?;
        Ok(Running(self.0))
    }
}

/// A delivered event whose handler is running. Dropping it lets the request finish in the
/// background.
pub struct Running(Request);

impl Running {
    /// Finishes the event as a scheduled event, as `IoContext::IncomingRequest::finishScheduled()`
    /// does: waits for the request's waitUntil tasks, up to the time limit of scheduled events.
    /// Returns its result: whether the event should be retried, and its outcome.
    ///
    /// The request finishes in the returned future, not in the background: dropping the future
    /// cancels it. An exception that `finishScheduled()` throws fails the future.
    pub fn finish_scheduled(
        mut self,
    ) -> impl Future<Output = Result<worker::ScheduledResult, KjException>> + 'static {
        let finished = bridge::invocation_finish_scheduled(self.0.take());
        async move { Ok(finished.await?.into()) }
    }
}
