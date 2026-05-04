// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;
use std::time::SystemTime;

use cxx::KjError;
use cxx::KjExceptionType;
use kj::http::ConnectResponse;
use kj::http::HttpConnectSettings;
use kj::http::HttpHeadersRef;
use kj::http::HttpMethod;
use kj::http::HttpServiceResponse;
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
        fn new_ok_worker() -> Box<Wrapper>;
    }
}

/// Test worker replying 200 "OK".
pub struct Worker;

impl Worker {
    fn not_implemented(name: &str) -> KjError {
        KjError::new(
            KjExceptionType::Unimplemented,
            format!("{name} not implemented"),
        )
    }
}

#[async_trait::async_trait(?Send)]
impl kj::http::HttpService for Worker {
    async fn request<'a>(
        &'a mut self,
        _method: HttpMethod,
        _url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        _request_body: Pin<&'a mut AsyncInputStream>,
        response: HttpServiceResponse<'a>,
    ) -> crate::Result<()> {
        let headers = headers.clone_shallow();
        let mut body = response.send(200, "OK", &headers, Some(2_u64))?;
        body.write(b"OK").await?;
        Ok(())
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HttpHeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: HttpConnectSettings<'a>,
    ) -> crate::Result<()> {
        Err(Self::not_implemented("connect"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Worker {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> crate::Result<ScheduledResult> {
        Err(Self::not_implemented("run_scheduled"))
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> crate::Result<AlarmResult> {
        Err(Self::not_implemented("run_alarm"))
    }

    async fn custom_event(
        &mut self,
        _event: Pin<&mut crate::CustomEvent>,
    ) -> crate::Result<crate::CustomEventResult> {
        Err(Self::not_implemented("custom_event"))
    }
}

pub fn new_ok_worker() -> Box<Wrapper> {
    Worker.into_ffi()
}
