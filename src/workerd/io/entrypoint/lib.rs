// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Rust [`WorkerEntrypoint`], which owns the C++ one, and the events it delivers from Rust.
//!
//! `WorkerEntrypoint` (`src/workerd/io/worker-entrypoint.h`) delivers each event to its
//! Worker's exported handler. For an event that Rust implements, the C++ object hands Rust an
//! [`Invocation`]: the request it was made for, and what finds the handler. Rust then reports the
//! event to the tracer, delivers it, runs the handler, and finishes the request.
//!
//! An invocation goes through three types, each of which is consumed by the step that leads to the
//! next, so that each step happens once and in order: an [`Invocation`] is delivered into a
//! [`Delivered`], whose handler is run into a [`Running`], which is finished. Every step calls C++
//! when it is called, not when a future it returns is first polled, so that the request is in the
//! state the step leaves it in as soon as the step returns, as in C++.
//!
//! See `AGENTS.md` for how the crate fits together and what is left to port.

mod fetch;
mod ffi;
mod scheduled;
mod test;

use std::future::Future;
use std::pin::Pin;

use cxx::KjException;
use ffi::bridge;
use io_context::RunError;
use jsg::v8;
use kj_rs::KjDate;
use kj_rs::KjOwn;

pub use crate::ffi::bridge::Invocation as InvocationFfi;

/// A `WorkerEntrypoint`: the receiver of one event for a Worker's exported handler, which makes
/// the `IoContext::IncomingRequest` the event runs in.
///
/// It owns the C++ `WorkerEntrypoint` (`src/workerd/io/worker-entrypoint.c++`), which made the
/// request. An event that Rust implements is delivered here, from the C++ object's
/// [`Invocation`]; any other event goes to the C++ object, which C++ calls directly (see
/// `newWorkerEntrypoint()` in `ffi.h`).
pub struct WorkerEntrypoint {
    cxx: KjOwn<bridge::CxxEntrypoint>,
}

impl WorkerEntrypoint {
    #[must_use]
    pub fn new(cxx: KjOwn<bridge::CxxEntrypoint>) -> Self {
        Self { cxx }
    }

    /// The C++ `WorkerEntrypoint`, which delivers the events that Rust doesn't implement.
    pub fn cxx(&mut self) -> Pin<&mut bridge::CxxEntrypoint> {
        self.cxx.pin_mut()
    }

    /// Takes the invocation of the entrypoint's one event, to deliver it.
    ///
    /// # Errors
    /// Fails if an event was already delivered.
    pub fn take_invocation(&mut self) -> Result<Invocation, KjException> {
        Ok(Invocation::new(bridge::cxx_entrypoint_take_invocation(
            self.cxx.pin_mut(),
        )?))
    }
}

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

    /// Fails if the event's request is for an actor, to which scheduled events are never
    /// delivered: they don't work with actors, whose cancellations happen at once, without
    /// waiting for the request's waitUntil tasks.
    ///
    /// # Errors
    /// Fails with the assertion failure.
    pub fn assert_no_actor(&mut self) -> Result<(), KjException> {
        io_context::incoming_request_assert_no_actor(ffi::request(self.invocation.pin_mut()))
    }

    /// Tells the request's tracer, if it has one, that the event is a custom event.
    ///
    /// # Errors
    /// Fails with the exception the tracer throws.
    pub fn set_custom_event_info(&mut self) -> Result<(), KjException> {
        io_context::incoming_request_set_custom_event_info(ffi::request(self.invocation.pin_mut()))
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
        io_context::incoming_request_set_scheduled_event_info(
            ffi::request(self.invocation.pin_mut()),
            scheduled_time,
            cron,
        )
    }

    /// Delivers the event: `IoContext::IncomingRequest::delivered()`. `event_type`, such as
    /// `"scheduled"`, names the request's async trace resource, and `location`, from
    /// [`io_context::delivered_location!`], is recorded as where the event's code delivered it.
    ///
    /// # Errors
    /// Fails with the exception that `delivered()` throws. The event is then dropped undelivered.
    pub fn deliver(
        mut self,
        event_type: &str,
        location: &'static io_context::DeliveredLocation,
    ) -> Result<Delivered, KjException> {
        io_context::incoming_request_delivered(
            ffi::request(self.invocation.pin_mut()),
            event_type,
            location,
        )?;
        Ok(Delivered(self.invocation))
    }
}

/// A delivered event, whose handler has not run.
///
/// Dropping it, as when a step fails, destroys the request without draining it, so that waitUntil
/// tasks the event started are cancelled. C++ does the same: when an exception leaves the event's
/// `WorkerEntrypoint` method, the request is destroyed with the `WorkerEntrypoint`.
pub struct Delivered(KjOwn<bridge::Invocation>);

/// The handles that [`Delivered::run_test_in_wait_until`] runs its function on.
pub struct TestHandler<'s> {
    /// The exported handler's `test()` function.
    pub test: v8::Local<'s, v8::Function>,
    /// The exported handler, which `test()` is a method of.
    pub handler: v8::Local<'s, v8::Value>,
    /// The Worker's env.
    pub env: v8::Local<'s, v8::Value>,
    /// The handler's ctx, or undefined if it has none.
    pub ctx: v8::Local<'s, v8::Value>,
}

impl Delivered {
    /// Runs `f` on the test event's [`TestHandler`] in a waitUntil task of the request, with
    /// [`io_context::incoming_request_run_in_wait_until`], then waits for the future that `f`
    /// returns. The handler and its `test()` function are found first, in the task, by the C++
    /// function `entrypoint::getTestHandles()`, as the run's C++ prelude, before Rust holds the
    /// lock: the task fails with what finding them throws, as when a class entrypoint's
    /// constructor throws, or what `ServiceWorkerGlobalScope::getTestHandler()` throws under
    /// Service Workers syntax or when there is no `test()` function. The prelude also names the
    /// run's Perfetto span, `WorkerEntrypoint::test() run`.
    ///
    /// # Errors
    /// Fails with the exception that keeps the task from being added.
    pub fn run_test_in_wait_until<F>(
        mut self,
        f: impl for<'s> FnOnce(&mut jsg::Lock<'s>, TestHandler<'s>) -> Result<F, RunError> + 'static,
    ) -> Result<Running, KjException>
    where
        F: Future<Output = Result<(), KjException>> + 'static,
    {
        let lookup = bridge::invocation_take_test_handler_lookup(self.0.pin_mut());
        io_context::incoming_request_run_in_wait_until(
            ffi::request(self.0.pin_mut()),
            lookup,
            move |lock, handles| {
                let &[test, handler, env, ctx] = handles else {
                    unreachable!("the test handler lookup returns test(), the handler, env and ctx")
                };
                let Ok(test) = v8::Local::<v8::Function>::try_from(test) else {
                    unreachable!("getTestHandler() returns a function")
                };
                f(
                    lock,
                    TestHandler {
                        test,
                        handler,
                        env,
                        ctx,
                    },
                )
            },
        )?;
        Ok(Running(self.0))
    }

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
    /// Fails with the exception that keeps the task from being added. The request is then
    /// destroyed.
    pub fn start_scheduled_in_wait_until(
        mut self,
        scheduled_time: KjDate,
        cron: &[u8],
    ) -> Result<Running, KjException> {
        bridge::invocation_start_scheduled_in_wait_until(self.0.pin_mut(), scheduled_time, cron)?;
        Ok(Running(self.0))
    }
}

/// A delivered event whose handler is running. Dropping it destroys the request, as for a
/// [`Delivered`].
pub struct Running(KjOwn<bridge::Invocation>);

impl Running {
    /// Finishes the event as a scheduled event, as `IoContext::IncomingRequest::finishScheduled()`
    /// does: waits for the request's waitUntil tasks, up to the time limit of scheduled events.
    /// Returns its result: whether the event should be retried, and its outcome.
    ///
    /// The request finishes in the returned future: dropping the future cancels it. An exception
    /// that `finishScheduled()` throws fails the future, where C++ throws it from
    /// `runScheduled()`: cxx can't carry a synchronous throw out of an async function. It asserts
    /// that the request has no actor and is the only one, so only a runtime bug reaches it.
    pub fn finish_scheduled(
        mut self,
    ) -> impl Future<Output = Result<worker::ScheduledResult, KjException>> + 'static {
        let finished = io_context::incoming_request_finish_scheduled(
            bridge::invocation_take_request(self.0.pin_mut()),
        );
        async move { Ok(finished.await?.into()) }
    }
}
