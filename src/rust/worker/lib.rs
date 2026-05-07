// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Worker interface crate.
//!
//! This crate contains Rust's counterpart of `workerd::WorkerInterface`, necessary ffi bindings
//! and several simple interface implementations.
//!
//! The intended way to use this crate is to `use worker` and reference `worker::Interface` trait
//! in your code.
//!
//! Current limitations:
//! - most parameters are opaque c++ types

pub mod error;
pub mod exception;
pub mod ffi;
pub mod kill_switch;
pub mod ok;

use std::pin::Pin;
use std::time::SystemTime;

use cxx::KjError;
pub use kj::http::ConnectResponse;
pub use kj::http::ConnectSettings;
pub use kj::http::HeaderId;
pub use kj::http::Headers;
pub use kj::http::HeadersRef;
pub use kj::http::Method;
pub use kj::http::Service;
pub use kj::http::ServiceResponse;
pub use kj::io::AsyncInputStream;
pub use kj::io::AsyncIoStream;
use outcome_capnp::EventOutcome;

pub use crate::ffi::Wrapper;
pub use crate::ffi::bridge::CustomEvent;

pub type Result<T> = std::result::Result<T, KjError>;

/// An interface representing the services made available by a worker/pipeline to handle a request.
/// Corresponds to `workerd::WorkerInterface`
#[async_trait::async_trait(?Send)]
pub trait Interface: kj::http::Service {
    /// Hints that this worker will likely be invoked in the near future, so should be warmed up now.
    /// This method should also call `prewarm()` on any subsequent pipeline stages that are expected
    /// to be invoked.
    ///
    /// If `prewarm()` has to do anything asynchronous, it should use "waitUntil" tasks.
    async fn prewarm(&mut self, _url: &str) -> Result<()> {
        Ok(())
    }

    /// Trigger a scheduled event with the given scheduled (unix timestamp) time and cron string.
    /// The cron string must be valid until the returned promise completes.
    /// Async work is queued in a "waitUntil" task set.
    async fn run_scheduled(
        &mut self,
        scheduled_time: &SystemTime,
        cron: &str,
    ) -> Result<ScheduledResult>;

    /// Trigger an alarm event with the given scheduled (unix timestamp) time.
    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> Result<AlarmResult>;

    /// Run the test handler. The returned promise resolves to true or false to indicate that the test
    /// passed or failed. In the case of a failure, information should have already been written to
    /// stderr and to the devtools; there is no need for the caller to write anything further. (If the
    /// promise rejects, this indicates a bug in the test harness itself.)
    async fn test(&mut self) -> Result<bool> {
        Ok(false)
    }

    /// Allows delivery of a variety of event types by implementing a callback that delivers the
    /// event to a particular isolate. If and when the event is delivered to an isolate,
    /// `callback->run()` will be called inside a fresh `IoContext::IncomingRequest` to begin the
    /// event.
    ///
    /// If the event needs to return some sort of result, it's the responsibility of the callback to
    /// store that result in a side object that the event's invoker can inspect after the promise has
    /// resolved.
    ///
    /// Note that it is guaranteed that if the returned promise is canceled, `event` will be dropped
    /// immediately; if its callbacks have not run yet, they will not run at all. So, a `CustomEvent`
    /// implementation can hold references to objects it doesn't own as long as the returned promise
    /// will be canceled before those objects go away.
    async fn custom_event(&mut self, event: Pin<&mut CustomEvent>) -> Result<CustomEventResult>;

    /// Convert `self` into the structure suitable for passing to C++ through FFI layer
    /// To obtain `workerd::WorkerInterface` on C++ side finish wrapping with `fromRust()`
    /// call defined in `bridge.h`.
    fn into_ffi(self) -> Box<ffi::Wrapper>
    where
        Self: Sized + 'static,
    {
        Box::new(ffi::Wrapper::new(Box::new(self)))
    }
}

/// Result of a scheduled event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScheduledResult {
    pub retry: bool,
    pub outcome: EventOutcome,
}

/// Result of an alarm event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AlarmResult {
    pub retry: bool,
    pub retry_counts_against_limit: bool,
    pub outcome: EventOutcome,
    pub error_description: Option<String>,
}

/// Result of a custom event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CustomEventResult {
    pub outcome: EventOutcome,
}

#[cfg(test)]
mod tests {
    use kj::http::HeaderId;

    #[expect(dead_code, reason = "HeaderOverrideProxy exists as compilation test.")]
    struct HeaderOverrideProxy(Box<dyn crate::Interface>);

    #[async_trait::async_trait(?Send)]
    impl kj::http::Service for HeaderOverrideProxy {
        async fn request<'a>(
            &'a mut self,
            method: kj::http::Method,
            url: &'a [u8],
            headers: kj::http::HeadersRef<'a>,
            request_body: std::pin::Pin<&'a mut kj::io::AsyncInputStream>,
            response: kj::http::ServiceResponse<'a>,
        ) -> kj::Result<()> {
            let mut headers = headers.clone_shallow();
            headers.set(HeaderId::HOST, "example.com");
            self.0
                .request(method, url, headers.as_ref(), request_body, response)
                .await?;
            Ok(())
        }

        async fn connect<'a>(
            &'a mut self,
            _host: &'a [u8],
            _headers: kj::http::HeadersRef<'a>,
            _connection: std::pin::Pin<&'a mut kj::io::AsyncIoStream>,
            _response: kj::http::ConnectResponse<'a>,
            _settings: kj::http::ConnectSettings<'a>,
        ) -> kj::Result<()> {
            todo!()
        }
    }
}
