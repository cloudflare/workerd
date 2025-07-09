// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust wrapper for WorkerInterface that can be exposed to C++
//!
//! This module provides a wrapper around `Box<dyn WorkerInterface>` that can be
//! used from C++ code via the CXX bridge.

use std::pin::Pin;

use crate::Error;
use crate::Result;
use crate::ffi::AlarmResult as CxxAlarmResult;
use crate::ffi::AsyncInputStream;
use crate::ffi::AsyncIoStream;
use crate::ffi::ConnectResponse;
use crate::ffi::CustomEvent;
use crate::ffi::CustomEventResult as CxxCustomEventResult;
use crate::ffi::Date;
use crate::ffi::HttpConnectSettings;
use crate::ffi::HttpHeaders;
use crate::ffi::HttpMethod;
use crate::ffi::HttpServiceResponse;
use crate::ffi::ScheduledResult as CxxScheduledResult;
use crate::interface::BoxedWorkerInterface;

/// Opaque wrapper around BoxedWorkerInterface that can be exposed to C++
pub struct RustWorkerInterface {
    inner: BoxedWorkerInterface,
}

impl RustWorkerInterface {
    /// Create a new wrapper from a BoxedWorkerInterface
    pub fn new(worker: BoxedWorkerInterface) -> Self {
        Self { inner: worker }
    }
}

/// Factory function to create wrapper (called from C++)
pub fn create_rust_worker_interface_wrapper() -> Box<RustWorkerInterface> {
    // For now, create a dummy wrapper - in practice this would be called with actual worker
    use crate::interface::from_exception;
    let error = Error::from("Not implemented");
    let worker = from_exception(error);
    Box::new(RustWorkerInterface::new(worker))
}

/// Factory function to create wrapper from BoxedWorkerInterface
pub fn create_rust_worker_interface_wrapper_from_boxed(
    worker: BoxedWorkerInterface,
) -> Box<RustWorkerInterface> {
    Box::new(RustWorkerInterface::new(worker))
}

/// Factory function to create exception worker (called from C++)
pub fn create_exception_worker(error_message: &str) -> Box<RustWorkerInterface> {
    use crate::interface::from_exception;
    let error = Error::from(error_message.to_string());
    let worker = from_exception(error);
    Box::new(RustWorkerInterface::new(worker))
}

impl RustWorkerInterface {
    pub async unsafe fn rust_worker_request<'a>(
        self: &'a mut RustWorkerInterface,
        method: &'a HttpMethod,
        url: &'a str,
        headers: &'a HttpHeaders,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut HttpServiceResponse>,
    ) -> Result<()> {
        self.inner
            .request(method, url, headers, request_body, response)
            .await?;
        Ok(())
    }

    /// Connect to a host (called from C++)
    pub async fn rust_worker_connect(
        self: &mut RustWorkerInterface,
        host: &str,
        headers: &HttpHeaders,
        connection: Pin<&mut AsyncIoStream>,
        response: Pin<&mut ConnectResponse>,
        settings: Pin<&mut HttpConnectSettings>,
    ) -> Result<()> {
        self.inner
            .connect(host, headers, connection, response, settings)
            .await?;
        Ok(())
    }

    /// Prewarm the worker (called from C++)
    pub async fn rust_worker_prewarm(self: &mut RustWorkerInterface, url: &str) -> Result<()> {
        self.inner.prewarm(url).await?;
        Ok(())
    }

    /// Run a scheduled event (called from C++)
    pub async fn rust_worker_run_scheduled(
        self: &mut RustWorkerInterface,
        scheduled_time: &Date,
        cron: &str,
    ) -> Result<CxxScheduledResult> {
        let result = self.inner.run_scheduled(scheduled_time, cron).await?;
        Ok(result)
    }

    /// Run an alarm event (called from C++)
    pub async fn rust_worker_run_alarm(
        self: &mut RustWorkerInterface,
        scheduled_time: &Date,
        retry_count: u32,
    ) -> Result<CxxAlarmResult> {
        let result = self.inner.run_alarm(scheduled_time, retry_count).await?;
        Ok(result)
    }

    /// Run the test handler (called from C++)
    pub async fn rust_worker_test(self: &mut RustWorkerInterface) -> Result<bool> {
        Err("Async operations not supported in CXX bridge".into())
    }

    /// Handle a custom event (called from C++)
    pub async fn rust_worker_custom_event(
        self: &mut RustWorkerInterface,
        event: Pin<&mut CustomEvent>,
    ) -> Result<CxxCustomEventResult> {
        let result = self.inner.custom_event(event).await?;
        Ok(result)
    }
}
