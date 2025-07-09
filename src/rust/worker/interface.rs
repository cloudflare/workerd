// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trait interface for WorkerInterface
//!
//! This module provides a Rust async trait interface that mirrors the C++ WorkerInterface
//! from `src/workerd/io/worker-interface.h`. An interface representing the services made
//! available by a worker/pipeline to handle a request.

use std::pin::Pin;

use async_trait::async_trait;

use crate::CustomEventResult;
use crate::Error;
use crate::HttpServiceResponse;
use crate::Result;
use crate::ffi::AlarmResult;
use crate::ffi::AsyncInputStream;
use crate::ffi::AsyncIoStream;
use crate::ffi::ConnectResponse;
use crate::ffi::CustomEvent;
use crate::ffi::Date;
use crate::ffi::HttpConnectSettings;
use crate::ffi::HttpHeaders;
use crate::ffi::HttpMethod;
use crate::ffi::ScheduledResult;

/// Trait analogous to the C++ `workerd::WorkerInterface: public kj::HttpService`
///
/// An interface representing the services made available by a worker/pipeline to handle a request.
#[async_trait(?Send)]
pub trait WorkerInterface {
    /// Make an HTTP request. (This method is inherited from HttpService, but re-declared here for
    /// visibility.)
    async fn request(
        &mut self,
        method: &HttpMethod,
        url: &str,
        headers: &HttpHeaders,
        request_body: Pin<&mut AsyncInputStream>,
        response: Pin<&mut HttpServiceResponse>,
    ) -> Result<()>;

    /// This is the same as the inherited HttpService::connect(), but we override it to be
    /// pure-virtual to force all subclasses of WorkerInterface to implement it explicitly rather
    /// than get the default implementation which throws an unimplemented exception.
    async fn connect(
        &mut self,
        host: &str,
        headers: &HttpHeaders,
        connection: Pin<&mut AsyncIoStream>,
        response: Pin<&mut ConnectResponse>,
        settings: Pin<&mut HttpConnectSettings>,
    ) -> Result<()>;

    /// Hints that this worker will likely be invoked in the near future, so should be warmed up now.
    /// This method should also call `prewarm()` on any subsequent pipeline stages that are expected
    /// to be invoked.
    ///
    /// If prewarm() has to do anything asynchronous, it should use "waitUntil" tasks.
    async fn prewarm(&mut self, url: &str) -> Result<()>;

    /// Trigger a scheduled event with the given scheduled (unix timestamp) time and cron string.
    /// The cron string must be valid until the returned promise completes.
    /// Async work is queued in a "waitUntil" task set.
    async fn run_scheduled(&mut self, scheduled_time: &Date, cron: &str)
    -> Result<ScheduledResult>;

    /// Trigger an alarm event with the given scheduled (unix timestamp) time.
    async fn run_alarm(&mut self, scheduled_time: &Date, retry_count: u32) -> Result<AlarmResult>;

    /// Run the test handler. The returned promise resolves to true or false to indicate that the test
    /// passed or failed. In the case of a failure, information should have already been written to
    /// stderr and to the devtools; there is no need for the caller to write anything further. (If the
    /// promise rejects, this indicates a bug in the test harness itself.)
    async fn test(&mut self) -> Result<bool> {
        Ok(false)
    }

    /// Allows delivery of a variety of event types by implementing a callback that delivers the
    /// event to a particular isolate. If and when the event is delivered to an isolate,
    /// `callback->run()` will be called inside a fresh IoContext::IncomingRequest to begin the
    /// event.
    ///
    /// If the event needs to return some sort of result, it's the responsibility of the callback to
    /// store that result in a side object that the event's invoker can inspect after the promise has
    /// resolved.
    ///
    /// Note that it is guaranteed that if the returned promise is canceled, `event` will be dropped
    /// immediately; if its callbacks have not run yet, they will not run at all. So, a CustomEvent
    /// implementation can hold references to objects it doesn't own as long as the returned promise
    /// will be canceled before those objects go away.
    async fn custom_event(&mut self, event: Pin<&mut CustomEvent>) -> Result<CustomEventResult>;
}

/// Trait object type for WorkerInterface
pub type BoxedWorkerInterface = Box<dyn WorkerInterface>;

/// Trait for creating promised worker interfaces
///
/// C++ equivalent: `kj::Own<WorkerInterface> newPromisedWorkerInterface(kj::Promise<kj::Own<WorkerInterface>> promise);`
#[async_trait(?Send)]
pub trait PromisedWorkerInterface {
    /// Resolve the promise and return the worker interface
    async fn resolve(self: Box<Self>) -> Result<BoxedWorkerInterface>;
}

/// Error type for WorkerInterface operations
#[derive(Debug)]
pub enum WorkerInterfaceError {
    /// The operation is not supported
    NotSupported,
    /// The worker is not available
    WorkerUnavailable,
    /// A timeout occurred
    Timeout,
    /// An internal error occurred
    Internal(String),
}

impl std::fmt::Display for WorkerInterfaceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WorkerInterfaceError::NotSupported => write!(f, "Operation not supported"),
            WorkerInterfaceError::WorkerUnavailable => write!(f, "Worker unavailable"),
            WorkerInterfaceError::Timeout => write!(f, "Operation timed out"),
            WorkerInterfaceError::Internal(msg) => write!(f, "Internal error: {}", msg),
        }
    }
}

impl std::error::Error for WorkerInterfaceError {}

/// Helper function to create a worker interface that always throws an exception
///
/// C++ equivalent: `static kj::Own<WorkerInterface> fromException(kj::Exception&& e);`
pub fn from_exception(error: Error) -> BoxedWorkerInterface {
    Box::new(ExceptionWorkerInterface::new(error))
}

/// Implementation of WorkerInterface that always throws an exception
///
/// Rust equivalent of C++ `WorkerInterface::fromException(kj::Exception&& e)`
struct ExceptionWorkerInterface {
    error: String,
}

impl ExceptionWorkerInterface {
    fn new(error: Error) -> Self {
        Self {
            error: error.to_string(),
        }
    }
}

#[async_trait(?Send)]
impl WorkerInterface for ExceptionWorkerInterface {
    async fn request(
        &mut self,
        _method: &HttpMethod,
        _url: &str,
        _headers: &HttpHeaders,
        _request_body: Pin<&mut AsyncInputStream>,
        _response: Pin<&mut HttpServiceResponse>,
    ) -> Result<()> {
        Err(self.error.clone().into())
    }

    async fn connect(
        &mut self,
        _host: &str,
        _headers: &HttpHeaders,
        _connection: Pin<&mut AsyncIoStream>,
        _response: Pin<&mut ConnectResponse>,
        _settings: Pin<&mut HttpConnectSettings>,
    ) -> Result<()> {
        Err(self.error.clone().into())
    }

    async fn prewarm(&mut self, _url: &str) -> Result<()> {
        Err(self.error.clone().into())
    }

    async fn run_scheduled(
        &mut self,
        _scheduled_time: &Date,
        _cron: &str,
    ) -> Result<ScheduledResult> {
        Err(self.error.clone().into())
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &Date,
        _retry_count: u32,
    ) -> Result<AlarmResult> {
        Err(self.error.clone().into())
    }

    async fn custom_event(&mut self, _event: Pin<&mut CustomEvent>) -> Result<CustomEventResult> {
        Err(self.error.clone().into())
    }
}
