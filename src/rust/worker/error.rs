// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;
use std::time::SystemTime;

use cxx::KjError;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;

use crate::AlarmResult;
use crate::Interface;
use crate::ScheduledResult;
use crate::ffi::Wrapper;

#[cxx::bridge(namespace = "workerd::rust::worker")]
pub mod bridge {
    extern "Rust" {
        type Wrapper = crate::ffi::Wrapper;

        #[expect(
            clippy::unnecessary_box_returns,
            reason = "c++ expects heap-allocation"
        )]
        fn new_error_worker(message: &str) -> Box<Wrapper>;
    }
}

/// Worker that returns errors for all methods.
pub struct Worker {
    message: String,
}

impl Worker {
    pub fn new(message: String) -> Self {
        Self { message }
    }

    fn error(&self, file: &str, line: u32) -> KjError {
        KjError::new(
            cxx::KjExceptionType::Failed,
            format!("jsg.Error: {}", self.message),
        )
        .with_location(file.to_owned(), line)
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
        Err(self.error(file!(), line!()))
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> crate::Result<()> {
        Err(self.error(file!(), line!()))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Worker {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> crate::Result<ScheduledResult> {
        Err(self.error(file!(), line!()))
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> crate::Result<AlarmResult> {
        Err(self.error(file!(), line!()))
    }

    async fn custom_event(
        &mut self,
        _event: Pin<&mut crate::CustomEvent>,
    ) -> crate::Result<crate::CustomEventResult> {
        Err(self.error(file!(), line!()))
    }
}

pub fn new_error_worker(message: &str) -> Box<Wrapper> {
    Worker::new(message.to_owned()).into_ffi()
}
