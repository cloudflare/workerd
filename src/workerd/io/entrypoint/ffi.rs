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
    reason = "the cxx bridge expands to unsafe FFI glue, and invocation_request() returns a \
              reference"
)]

use std::future::Future;
use std::pin::Pin;

use cxx::KjException;
use kj_rs::KjDate;
use kj_rs::KjOwn;

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
    }

    unsafe extern "C++" {
        include!("workerd/io/entrypoint/ffi.h");

        type Invocation;

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

fn scheduled_event_start(
    invocation: KjOwn<bridge::Invocation>,
    scheduled_time: KjDate,
    cron: &[u8],
) -> Result<Box<ScheduledEvent>, KjException> {
    let finished =
        crate::scheduled::start(crate::Invocation::new(invocation), scheduled_time, cron)?;
    Ok(Box::new(ScheduledEvent(Box::pin(async move {
        Ok(finished.await?.into())
    }))))
}

async fn scheduled_event_finish(
    event: Box<ScheduledEvent>,
) -> Result<bridge::ScheduledResult, KjException> {
    event.0.await
}
