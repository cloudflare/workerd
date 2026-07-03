// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `CxxWorkerInterface`: wraps a C++ `workerd::WorkerInterface` and exposes it as a Rust
//! `worker::Interface`.
//!
//! Lets Rust code delegate to (decorate) a C++ worker. This is the reverse of `RustWorkerInterface`
//! (bridge.h), which exposes a Rust `worker::Interface` to C++.

use std::pin::Pin;
use std::time::SystemTime;

use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjDate;
use kj_rs::KjOwn;
use outcome_capnp::EventOutcome;

use crate::AlarmResult;
use crate::CustomEvent;
use crate::CustomEventResult;
use crate::Interface;
use crate::Result;
use crate::ScheduledResult;
use crate::ffi::bridge;

/// Wraps an owned C++ `WorkerInterface`, delegating every event to it.
pub struct CxxWorkerInterface {
    inner: KjOwn<bridge::WorkerInterface>,
}

impl CxxWorkerInterface {
    #[must_use]
    pub fn new(inner: KjOwn<bridge::WorkerInterface>) -> Self {
        Self { inner }
    }
}

#[async_trait::async_trait(?Send)]
impl Service for CxxWorkerInterface {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        // SAFETY: the returned future borrows self.inner and the arguments for its duration.
        unsafe {
            bridge::worker_request(
                self.inner.as_mut(),
                method,
                url,
                headers.as_ffi(),
                request_body,
                response.into_ffi(),
            )
        }
        .await?;
        Ok(())
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Result<()> {
        // SAFETY: the returned future borrows self.inner and the arguments for its duration.
        unsafe {
            bridge::worker_connect(
                self.inner.as_mut(),
                host,
                headers.as_ffi(),
                connection,
                response.into_ffi(),
                settings,
            )
        }
        .await?;
        Ok(())
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for CxxWorkerInterface {
    async fn prewarm(&mut self, url: &str) -> Result<()> {
        // SAFETY: the returned future borrows self.inner for its duration.
        unsafe { bridge::worker_prewarm(self.inner.as_mut(), url.as_bytes()) }.await?;
        Ok(())
    }

    async fn run_scheduled(
        &mut self,
        scheduled_time: &SystemTime,
        cron: &str,
    ) -> Result<ScheduledResult> {
        let nanos = KjDate::from(*scheduled_time).nanoseconds();
        // SAFETY: the returned future borrows self.inner for its duration.
        let result =
            unsafe { bridge::worker_run_scheduled(self.inner.as_mut(), nanos, cron.as_bytes()) }
                .await?;
        Ok(result.into())
    }

    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> Result<AlarmResult> {
        let nanos = KjDate::from(*scheduled_time).nanoseconds();
        // SAFETY: the returned future borrows self.inner for its duration.
        let result =
            unsafe { bridge::worker_run_alarm(self.inner.as_mut(), nanos, retry_count) }.await?;
        Ok(result.into())
    }

    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        // SAFETY: the returned future borrows self.inner for its duration; event is moved in.
        let result = unsafe { bridge::worker_custom_event(self.inner.as_mut(), event) }.await?;
        Ok(result.into())
    }

    async fn test(&mut self) -> Result<bool> {
        // SAFETY: the returned future borrows self.inner for its duration.
        Ok(unsafe { bridge::worker_test(self.inner.as_mut()) }.await?)
    }
}

// Reverse of the conversions in ffi.rs: the bridge's shared result structs -> the crate's owned
// result types.
impl From<bridge::ScheduledResult> for ScheduledResult {
    fn from(value: bridge::ScheduledResult) -> Self {
        Self {
            retry: value.retry,
            outcome: value.outcome.into(),
        }
    }
}

impl From<bridge::AlarmResult> for AlarmResult {
    fn from(value: bridge::AlarmResult) -> Self {
        Self {
            retry: value.retry,
            retry_counts_against_limit: value.retry_counts_against_limit,
            outcome: value.outcome.into(),
            error_description: value.error_description.into(),
        }
    }
}

impl From<bridge::CustomEventResult> for CustomEventResult {
    fn from(value: bridge::CustomEventResult) -> Self {
        Self {
            outcome: value.outcome.into(),
        }
    }
}

impl From<bridge::EventOutcome> for EventOutcome {
    fn from(value: bridge::EventOutcome) -> Self {
        match value {
            bridge::EventOutcome::Unknown => Self::Unknown,
            bridge::EventOutcome::Ok => Self::Ok,
            bridge::EventOutcome::Exception => Self::Exception,
            bridge::EventOutcome::ExceededCpu => Self::ExceededCpu,
            bridge::EventOutcome::KillSwitch => Self::KillSwitch,
            bridge::EventOutcome::DaemonDown => Self::DaemonDown,
            bridge::EventOutcome::ScriptNotFound => Self::ScriptNotFound,
            bridge::EventOutcome::Canceled => Self::Canceled,
            bridge::EventOutcome::ExceededMemory => Self::ExceededMemory,
            bridge::EventOutcome::LoadShed => Self::LoadShed,
            bridge::EventOutcome::ResponseStreamDisconnected => Self::ResponseStreamDisconnected,
            bridge::EventOutcome::InternalError => Self::InternalError,
            bridge::EventOutcome::ExceededWallTime => Self::ExceededWallTime,
            other => {
                unreachable!(
                    "unknown WorkerInterface::EventOutcome discriminant: {}",
                    other.repr
                )
            }
        }
    }
}
