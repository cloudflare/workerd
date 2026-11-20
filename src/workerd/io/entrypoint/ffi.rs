// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ calls in `ffi.h`, and the events that C++ hands to Rust through it.
//!
//! Each C++ call does its work when it is called, an async one included: it returns a future for
//! what remains to wait for. A panic in a Rust function that C++ calls reaches C++ as a KJ
//! exception, through the cxx bridge; a future that C++ polls and drops is wrapped in
//! [`CatchPanics`], since kj-rs polls and drops it through `extern "C"` functions, where a panic
//! would abort the process.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and invocation_delivered() keeps the event \
              type it is given"
)]

use std::ffi::CStr;
use std::future::Future;
use std::pin::Pin;

use cxx::KjException;
use io_context::CatchPanics;
use kj_rs::KjDate;
use kj_rs::KjOwn;

#[cxx::bridge(namespace = "workerd::entrypoint")]
pub mod bridge {
    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.rs.h");

        type ScheduledResult = worker::ffi::bridge::ScheduledResult;
    }

    unsafe extern "C++" {
        include!("workerd/io/entrypoint/ffi.h");

        type Invocation;

        fn invocation_set_scheduled_event_info(
            invocation: Pin<&mut Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;
        /// # Safety
        /// `event_type` is a static NUL-terminated string with the NUL, which C++ keeps. (C++
        /// checks that the NUL is its only one.)
        unsafe fn invocation_delivered(
            invocation: Pin<&mut Invocation>,
            event_type: &[u8],
        ) -> Result<()>;
        fn invocation_drain(invocation: KjOwn<Invocation>);
        fn invocation_start_scheduled_in_wait_until(
            invocation: Pin<&mut Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<()>;
        async fn invocation_finish_scheduled(
            invocation: KjOwn<Invocation>,
        ) -> Result<ScheduledResult>;
    }

    extern "Rust" {
        type ScheduledEvent;

        /// Starts the scheduled event of `invocation`: see `runScheduled()` in `ffi.h`.
        fn scheduled_event_start(
            invocation: KjOwn<Invocation>,
            scheduled_time: KjDate,
            cron: &[u8],
        ) -> Result<Box<ScheduledEvent>>;
        async fn scheduled_event_finish(event: Box<ScheduledEvent>) -> Result<ScheduledResult>;
    }
}

/// Delivers `invocation` as an event of type `event_type`: see `invocation_delivered()` in `ffi.h`.
pub fn delivered(
    invocation: Pin<&mut bridge::Invocation>,
    event_type: &'static CStr,
) -> Result<(), KjException> {
    // SAFETY: `event_type` is a static NUL-terminated string with the NUL. A `CStr` has no other
    // NUL.
    unsafe { bridge::invocation_delivered(invocation, event_type.to_bytes_with_nul()) }
}

/// What a scheduled event that Rust has started finishes with.
type ScheduledFinish = Pin<Box<dyn Future<Output = Result<bridge::ScheduledResult, KjException>>>>;

/// A scheduled event that Rust has started, until it finishes.
pub struct ScheduledEvent(CatchPanics<ScheduledFinish>);

fn scheduled_event_start(
    invocation: KjOwn<bridge::Invocation>,
    scheduled_time: KjDate,
    cron: &[u8],
) -> Result<Box<ScheduledEvent>, KjException> {
    let finished =
        crate::scheduled::start(crate::Invocation::new(invocation), scheduled_time, cron)?;
    Ok(Box::new(ScheduledEvent(CatchPanics::new(
        Box::pin(async move { Ok(finished.await?.into()) }),
        "the future of a scheduled event",
    ))))
}

async fn scheduled_event_finish(
    event: Box<ScheduledEvent>,
) -> Result<bridge::ScheduledResult, KjException> {
    event.0.await?
}
