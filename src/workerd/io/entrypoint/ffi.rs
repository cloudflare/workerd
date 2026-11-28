// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ calls in `ffi.h`, and the events that C++ hands to Rust through it.
//!
//! Each C++ call does its work when it is called, an async one included: it returns a future for
//! what remains to wait for. A panic in a Rust function that C++ calls reaches C++ as a KJ
//! exception, through the cxx bridge, and kj-rs turns a panic while polling or dropping a future
//! that C++ waits for into a KJ exception or a log line.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, invocation_request() returns a \
              reference, and C++ keeps the reference that WorkerEntrypoint::cxx() returns"
)]

use std::future::Future;
use std::pin::Pin;

use cxx::KjException;
use kj_rs::KjDate;
use kj_rs::KjOwn;

use crate::WorkerEntrypoint;

#[cxx::bridge(namespace = "workerd::entrypoint")]
pub mod bridge {
    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.rs.h");

        type ScheduledResult = worker::ffi::bridge::ScheduledResult;
    }

    #[namespace = "workerd::io_context"]
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        type IncomingRequest = io_context::IncomingRequest;
        type RunPrelude = io_context::RunPrelude;
    }

    unsafe extern "C++" {
        include!("workerd/io/entrypoint/ffi.h");

        type Invocation;
        type CxxEntrypoint;
        type Fetch;

        fn fetch_set_event_info(fetch: Pin<&mut Fetch>) -> Result<()>;
        /// The event's request, until `fetch_end_handler()` drains it; then it fails.
        fn fetch_request<'a>(fetch: Pin<&'a mut Fetch>) -> Result<Pin<&'a mut IncomingRequest>>;
        fn fetch_delivered(fetch: Pin<&mut Fetch>) -> Result<()>;
        async fn fetch_run_handler<'a>(fetch: Pin<&'a mut Fetch>) -> Result<()>;
        async fn fetch_handler_failed<'a>(
            fetch: Pin<&'a mut Fetch>,
            exception: KjException,
        ) -> Result<()>;
        fn fetch_end_handler(fetch: Pin<&mut Fetch>) -> Result<()>;
        async fn fetch_finish_proxying<'a>(fetch: Pin<&'a mut Fetch>) -> Result<()>;
        fn fetch_prepare_failure(
            fetch: Pin<&mut Fetch>,
            exception: KjException,
        ) -> Result<KjException>;
        fn fetch_is_actor(fetch: &Fetch) -> bool;
        fn fetch_has_fail_open_service(fetch: &Fetch) -> bool;
        fn fetch_tunnels_exceptions(fetch: &Fetch) -> bool;
        fn fetch_propagate_from_actor(
            fetch: Pin<&mut Fetch>,
            exception: KjException,
        ) -> Result<KjException>;
        async fn fetch_fail_open<'a>(
            fetch: Pin<&'a mut Fetch>,
            exception: KjException,
        ) -> Result<()>;
        fn fetch_propagate(fetch: Pin<&mut Fetch>, exception: KjException) -> Result<KjException>;
        fn fetch_respond_with_error(fetch: Pin<&mut Fetch>, exception: KjException) -> Result<()>;

        fn cxx_entrypoint_take_invocation(
            entrypoint: Pin<&mut CxxEntrypoint>,
        ) -> Result<KjOwn<Invocation>>;

        /// # Safety
        /// The request has not been taken out of `invocation`.
        unsafe fn invocation_request<'a>(
            invocation: Pin<&'a mut Invocation>,
        ) -> Pin<&'a mut IncomingRequest>;
        fn invocation_take_request(invocation: Pin<&mut Invocation>) -> KjOwn<IncomingRequest>;
        fn invocation_start_scheduled_in_wait_until(
            invocation: Pin<&mut Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;

        fn invocation_take_test_handler_lookup(
            invocation: Pin<&mut Invocation>,
        ) -> KjOwn<RunPrelude>;
    }

    extern "Rust" {
        type WorkerEntrypoint;

        /// The Rust WorkerEntrypoint that owns `cxx`: see `newWorkerEntrypoint()` in `ffi.h`.
        #[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
        fn worker_entrypoint_new(cxx: KjOwn<CxxEntrypoint>) -> Box<WorkerEntrypoint>;
        /// The C++ WorkerEntrypoint that `self` owns.
        ///
        /// # Safety
        /// The reference is valid while `self` is: C++ keeps it no longer than the `Box` that
        /// holds `self`.
        #[expect(
            clippy::needless_lifetimes,
            reason = "the cxx bridge requires the lifetime of a returned reference"
        )]
        unsafe fn cxx<'a>(self: &'a mut WorkerEntrypoint) -> Pin<&'a mut CxxEntrypoint>;

        type ScheduledEvent;

        /// Starts the scheduled event of `entrypoint`, for `scheduled_time` and the cron pattern
        /// `cron`: takes its invocation, reports it to the tracer, delivers it and starts the
        /// handler in a waitUntil task (see `scheduled.rs`). Fails with the exception of a step
        /// that fails.
        fn scheduled_event_start(
            entrypoint: &mut WorkerEntrypoint,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<Box<ScheduledEvent>>;
        /// Waits for the request of a scheduled event to finish, up to the time limit of
        /// scheduled events. Dropping the future cancels the request.
        async fn scheduled_event_finish(event: Box<ScheduledEvent>) -> Result<ScheduledResult>;

        type FetchEvent;

        /// Starts the fetch event `fetch`: delivers it (see `fetch.rs`). Fails with the exception
        /// that delivering it fails with.
        fn fetch_event_start(fetch: KjOwn<Fetch>) -> Result<Box<FetchEvent>>;
        /// Runs the rest of a fetch event: its handler, its deferred proxying, and the handling of
        /// their failure. Dropping the future cancels the event.
        async fn fetch_event_finish(event: Box<FetchEvent>) -> Result<()>;

        type TestEvent;

        /// Starts the test event of `invocation`: see `runTest()` in `ffi.h`.
        fn test_event_start(invocation: KjOwn<Invocation>) -> Result<Box<TestEvent>>;
        async fn test_event_finish(event: Box<TestEvent>) -> Result<bool>;
    }
}

/// The request of `invocation`: see `invocation_request()` in `ffi.h`.
pub fn request(invocation: Pin<&mut bridge::Invocation>) -> Pin<&mut bridge::IncomingRequest> {
    // SAFETY: the typestates in `lib.rs` take the request out only when they finish it, after
    // which they call nothing else on the invocation.
    unsafe { bridge::invocation_request(invocation) }
}

/// What a scheduled event that Rust has started finishes with.
type ScheduledFinish = Pin<Box<dyn Future<Output = Result<bridge::ScheduledResult, KjException>>>>;

/// A scheduled event that Rust has started, until it finishes.
pub struct ScheduledEvent(ScheduledFinish);

#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
fn worker_entrypoint_new(cxx: KjOwn<bridge::CxxEntrypoint>) -> Box<WorkerEntrypoint> {
    Box::new(WorkerEntrypoint::new(cxx))
}

fn scheduled_event_start(
    entrypoint: &mut WorkerEntrypoint,
    scheduled_time: KjDate,
    cron: &[u8],
) -> Result<Box<ScheduledEvent>, KjException> {
    let finished = crate::scheduled::start(entrypoint, scheduled_time, cron)?;
    Ok(Box::new(ScheduledEvent(Box::pin(async move {
        Ok(finished.await?.into())
    }))))
}

async fn scheduled_event_finish(
    event: Box<ScheduledEvent>,
) -> Result<bridge::ScheduledResult, KjException> {
    event.0.await
}

/// What a test event that Rust has started finishes with: whether the test passed.
type TestFinish = Pin<Box<dyn Future<Output = Result<bool, KjException>>>>;

/// A test event that Rust has started, until it finishes.
pub struct TestEvent(TestFinish);

fn test_event_start(invocation: KjOwn<bridge::Invocation>) -> Result<Box<TestEvent>, KjException> {
    let finished = crate::test::start(crate::Invocation::new(invocation))?;
    Ok(Box::new(TestEvent(Box::pin(finished))))
}

async fn test_event_finish(event: Box<TestEvent>) -> Result<bool, KjException> {
    event.0.await
}

/// What a fetch event that Rust has started finishes with.
type FetchFinish = Pin<Box<dyn Future<Output = Result<(), KjException>>>>;

/// A fetch event that Rust has started, until it finishes.
pub struct FetchEvent(FetchFinish);

fn fetch_event_start(fetch: KjOwn<bridge::Fetch>) -> Result<Box<FetchEvent>, KjException> {
    let finished = crate::fetch::start(fetch)?;
    Ok(Box::new(FetchEvent(Box::pin(finished))))
}

async fn fetch_event_finish(event: Box<FetchEvent>) -> Result<(), KjException> {
    event.0.await
}
