// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The events of a C++ `WorkerEntrypoint` that Rust delivers.
//!
//! `WorkerEntrypoint` (`src/workerd/io/worker-entrypoint.h`) delivers each event to its
//! Worker's exported handler. For an event that Rust implements, it hands Rust an [`Invocation`]: the
//! request it was made for, and what finds the handler. Rust then reports the event to the
//! tracer, delivers it, runs the handler, and lets the request finish, either in the background or
//! by waiting for it, as for a scheduled event.
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
mod test;

use std::cell::Cell;
use std::ffi::CStr;
use std::future::Future;
use std::rc::Rc;

use cxx::KjException;
use ffi::bridge;
use jsg::v8;
use kj_rs::KjDate;
use kj_rs::KjOwn;

pub use crate::ffi::bridge::Invocation as InvocationFfi;

/// An invocation of a `WorkerEntrypoint` for an event, which it handed to Rust, not delivered yet. Dropping it cancels the
/// event before delivery, so that no JavaScript runs for it.
pub struct Invocation {
    invocation: KjOwn<bridge::Invocation>,
}

impl Invocation {
    #[must_use]
    pub fn new(invocation: KjOwn<bridge::Invocation>) -> Self {
        Self { invocation }
    }

    /// Tells the request's tracer, if it has one, that the event is a custom event.
    ///
    /// # Errors
    /// Fails with the exception the tracer throws.
    pub fn set_custom_event_info(&mut self) -> Result<(), KjException> {
        bridge::invocation_set_custom_event_info(self.invocation.pin_mut())
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

/// The handles that [`Delivered::run_in_wait_until`] runs its callback on.
pub struct Handler<'s> {
    /// The event's exported handler, or undefined if there is none, as under Service Workers
    /// syntax.
    pub handler: v8::Local<'s, v8::Value>,
    /// The Worker's env, or undefined if there is no exported handler.
    pub env: v8::Local<'s, v8::Value>,
    /// The handler's ctx, or undefined if it has none.
    pub ctx: v8::Local<'s, v8::Value>,
}

/// How a callback that [`Delivered::run_in_wait_until`] runs fails.
#[derive(Debug)]
pub enum RunError {
    /// A JavaScript error, which the run throws, unless it stands for an exception that is
    /// pending (see [`jsg::Error::is_pending`]). `IoContext::run()` then fails with the exception,
    /// which it logs as uncaught.
    Js(jsg::Error),
    /// A KJ exception, as a C++ call fails with, which `IoContext::run()` fails with unchanged.
    Kj(KjException),
}

impl From<jsg::Error> for RunError {
    fn from(err: jsg::Error) -> Self {
        Self::Js(err)
    }
}

impl From<KjException> for RunError {
    fn from(err: KjException) -> Self {
        Self::Kj(err)
    }
}

impl Delivered {
    /// Runs `f` in a waitUntil task of the request, as C++ `context.addWaitUntil(context.run(...))`
    /// does, under the Perfetto span `span_name`: in the request's `IoContext`, with the isolate
    /// locked and the request's async trace scopes entered, on the event's [`Handler`]. Once `f`
    /// has run, the task waits for the future `f` returns, outside the isolate lock. The task
    /// fails with the error `f` fails with, as `IoContext::run()` converts it, or with the error
    /// of the future.
    ///
    /// # Errors
    /// Fails with the exception that keeps the task from being added. The request then finishes
    /// in the background.
    pub fn run_in_wait_until<F>(
        mut self,
        span_name: &'static CStr,
        f: impl for<'s> FnOnce(&mut jsg::Lock<'s>, Handler<'s>) -> Result<F, RunError> + 'static,
    ) -> Result<Running, KjException>
    where
        F: Future<Output = Result<(), KjException>> + 'static,
    {
        let output = Rc::new(Cell::new(None));
        let sink = Rc::clone(&output);
        let callback = ffi::HandlerCallback::new(Box::new(move |lock, handler| {
            sink.set(Some(f(lock, handler)?));
            Ok(())
        }));
        // Waited for only once the callback has run, so the future is there.
        let then = ffi::WaitUntilContinuation::new(async move {
            let future = output.take().ok_or_else(|| -> KjException {
                kj::failed!("the callback of an entrypoint event did not run").into()
            })?;
            future.await
        });
        ffi::run_in_wait_until(self.0.invocation_mut().pin_mut(), span_name, callback, then)?;
        Ok(Running(self.0))
    }

    /// Starts the event's scheduled handler with a `ScheduledEvent` for `scheduled_time` and
    /// `cron`, in a waitUntil task of the request, in C++, as `WorkerEntrypoint::runScheduled()`
    /// does: `ServiceWorkerGlobalScope::startScheduled()` calls the exported handler's
    /// `scheduled()`, or under Service Workers syntax dispatches the event to its `scheduled`
    /// listeners, and makes the handler's promise a waitUntil task. The task fails with the
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
    /// does, under the Perfetto span `span_name`: waits for the request's waitUntil tasks, up to
    /// the time limit of scheduled events.
    /// Returns its result: whether the event should be retried, and its outcome.
    ///
    /// The request finishes in the returned future, not in the background: dropping the future
    /// cancels it.
    pub fn finish_scheduled(
        mut self,
        span_name: &'static CStr,
    ) -> impl Future<Output = Result<worker::ScheduledResult, KjException>> + 'static {
        let finished = ffi::finish_scheduled(self.0.take(), span_name);
        async move { Ok(finished.await?.into()) }
    }
}
