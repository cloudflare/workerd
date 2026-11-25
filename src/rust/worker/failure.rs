// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue"
)]

//! The worker that `WorkerInterface::fromException()` makes: one that fails every event with the
//! same exception.

use std::pin::Pin;
use std::time::SystemTime;

use cxx::KjException;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;

use crate::AlarmResult;
use crate::CustomEvent;
use crate::CustomEventResult;
use crate::Interface;
use crate::KjOwn;
use crate::ScheduledResult;
use crate::ffi::Wrapper;
use crate::ffi::bridge::custom_event_failed;

#[cxx::bridge(namespace = "workerd::rust::worker")]
pub mod bridge {
    extern "Rust" {
        type Wrapper = crate::ffi::Wrapper;

        #[expect(
            clippy::unnecessary_box_returns,
            reason = "c++ expects heap-allocation"
        )]
        fn new_failure_worker(exception: KjException) -> Box<Wrapper>;
    }
}

/// Fails every event with a copy of one exception.
///
/// Each copy keeps everything the exception holds. `prewarm()` succeeds, since there is nothing to
/// warm up; a custom event is told the exception with `failed()` before it fails.
pub struct Worker {
    exception: KjException,
}

impl Worker {
    #[must_use]
    pub fn new(exception: KjException) -> Self {
        Self { exception }
    }

    fn fail<T>(&self) -> crate::Result<T> {
        Err(self.exception.clone())
    }
}

#[async_trait::async_trait(?Send)]
impl kj::http::Service for Worker {
    async fn request<'a>(
        &'a mut self,
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _request_body: Pin<&'a mut AsyncInputStream>,
        _response: ServiceResponse<'a>,
    ) -> crate::Result<()> {
        self.fail()
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> crate::Result<()> {
        self.fail()
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Worker {
    async fn prewarm(&mut self, _url: &str) -> crate::Result<()> {
        Ok(())
    }

    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> crate::Result<ScheduledResult> {
        self.fail()
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> crate::Result<AlarmResult> {
        self.fail()
    }

    async fn abandon_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
    ) -> crate::Result<Option<SystemTime>> {
        self.fail()
    }

    async fn test(&mut self) -> crate::Result<bool> {
        self.fail()
    }

    /// If `failed()` itself throws, that exception is returned instead.
    async fn custom_event(
        &mut self,
        event: KjOwn<CustomEvent>,
    ) -> crate::Result<CustomEventResult> {
        custom_event_failed(event, self.exception.clone())?;
        self.fail()
    }
}

pub fn new_failure_worker(exception: KjException) -> Box<Wrapper> {
    Worker::new(exception).into_ffi()
}
