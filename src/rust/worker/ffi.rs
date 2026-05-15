// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use kj::http::ConnectSettings;
use kj_rs::KjDate;

use crate::CustomEvent;
use crate::Result;
use crate::ffi::bridge::AlarmResult;
use crate::ffi::bridge::CustomEventResult;
use crate::ffi::bridge::ScheduledResult;

#[cxx::bridge(namespace = "workerd::rust::worker")]
pub mod bridge {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        type HttpMethod = kj::http::ffi::HttpMethod;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type AsyncInputStream = kj::io::ffi::AsyncInputStream;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum EventOutcome {
        Unknown = 0,
        Ok = 1,
        Exception = 2,
        ExceededCpu = 3,
        KillSwitch = 4,
        DaemonDown = 5,
        ScriptNotFound = 6,
        Canceled = 7,
        ExceededMemory = 8,
        LoadShed = 9,
        ResponseStreamDisconnected = 10,
        InternalError = 11,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    // keep in sync with `src/workerd/io/worker-interface.h`
    pub struct ScheduledResult {
        pub retry: bool,
        pub outcome: EventOutcome,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    // keep in sync with `src/workerd/io/worker-interface.h`
    pub struct AlarmResult {
        pub retry: bool,
        pub retry_counts_against_limit: bool,
        pub outcome: EventOutcome,
        pub error_description: String,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    // keep in sync with `src/workerd/io/worker-interface.h`
    pub struct CustomEventResult {
        pub outcome: EventOutcome,
    }

    #[namespace = "kj::rust"]
    extern "C++" {
        type HttpConnectSettings<'a> = kj::http::ConnectSettings<'a>;
    }

    extern "Rust" {
        type Wrapper;

        async unsafe fn request<'a>(
            self: &'a mut Wrapper,
            method: HttpMethod,
            url: &'a [u8],
            headers: &'a HttpHeaders,
            request_body: Pin<&'a mut AsyncInputStream>,
            response: Pin<&'a mut HttpServiceResponse>,
        ) -> Result<()>;

        async unsafe fn connect<'a>(
            self: &'a mut Wrapper,
            host: &'a [u8],
            headers: &'a HttpHeaders,
            connection: Pin<&'a mut AsyncIoStream>,
            response: Pin<&'a mut ConnectResponse>,
            settings: HttpConnectSettings<'a>,
        ) -> Result<()>;

        async unsafe fn prewarm<'a>(self: &'a mut Wrapper, url: &'a [u8]) -> Result<()>;

        async unsafe fn run_scheduled<'a>(
            self: &'a mut Wrapper,
            scheduled_time: KjDate,
            cron: &'a [u8],
        ) -> Result<ScheduledResult>;

        async unsafe fn run_alarm<'a>(
            self: &'a mut Wrapper,
            scheduled_time: KjDate,
            retry_count: u32,
        ) -> Result<AlarmResult>;

        async unsafe fn custom_event<'a>(
            self: &'a mut Wrapper,
            event: Pin<&'a mut CustomEvent>,
        ) -> Result<CustomEventResult>;

        async unsafe fn test<'a>(self: &'a mut Wrapper) -> Result<bool>;
    }

    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.h");
    }

    unsafe extern "C++" {
        type CustomEvent;
    }

    impl Box<Wrapper> {}
}

pub struct Wrapper {
    worker: Box<dyn crate::Interface>,
}

impl Wrapper {
    pub(crate) fn new(worker: Box<dyn crate::Interface>) -> Self {
        Self { worker }
    }

    async fn request<'a>(
        &'a mut self,
        method: bridge::HttpMethod,
        url: &'a [u8],
        headers: &'a bridge::HttpHeaders,
        request_body: Pin<&'a mut bridge::AsyncInputStream>,
        response: Pin<&'a mut bridge::HttpServiceResponse>,
    ) -> Result<()> {
        let response = crate::ServiceResponse::from(response);
        self.worker
            .request(method, url, headers.into(), request_body, response)
            .await?;
        Ok(())
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: &'a bridge::HttpHeaders,
        connection: Pin<&'a mut bridge::AsyncIoStream>,
        response: Pin<&'a mut bridge::ConnectResponse>,
        settings: ConnectSettings<'a>,
    ) -> Result<()> {
        let response = crate::ConnectResponse::from(response);
        self.worker
            .connect(host, headers.into(), connection, response, settings)
            .await?;
        Ok(())
    }

    async fn prewarm(&mut self, url: &[u8]) -> Result<()> {
        self.worker.prewarm(&String::from_utf8_lossy(url)).await?;
        Ok(())
    }

    async fn run_scheduled(
        &mut self,
        scheduled_time: KjDate,
        cron: &[u8],
    ) -> Result<ScheduledResult> {
        let scheduled_time = scheduled_time.into();
        let result = self
            .worker
            .run_scheduled(&scheduled_time, &String::from_utf8_lossy(cron))
            .await?;
        Ok(result.into())
    }

    async fn run_alarm(&mut self, scheduled_time: KjDate, retry_count: u32) -> Result<AlarmResult> {
        let scheduled_time = scheduled_time.into();
        let result = self.worker.run_alarm(&scheduled_time, retry_count).await?;
        Ok(result.into())
    }

    async fn custom_event(&mut self, event: Pin<&mut CustomEvent>) -> Result<CustomEventResult> {
        let result = self.worker.custom_event(event).await?;
        Ok(result.into())
    }

    async fn test(&mut self) -> Result<bool> {
        let result = self.worker.test().await?;
        Ok(result)
    }
}

impl From<crate::ScheduledResult> for bridge::ScheduledResult {
    fn from(value: crate::ScheduledResult) -> Self {
        Self {
            retry: value.retry,
            outcome: value.outcome.into(),
        }
    }
}

impl From<crate::AlarmResult> for bridge::AlarmResult {
    fn from(value: crate::AlarmResult) -> Self {
        Self {
            retry: value.retry,
            retry_counts_against_limit: value.retry_counts_against_limit,
            outcome: value.outcome.into(),
            error_description: value.error_description.unwrap_or_default(),
        }
    }
}

impl From<crate::CustomEventResult> for bridge::CustomEventResult {
    fn from(value: crate::CustomEventResult) -> Self {
        Self {
            outcome: value.outcome.into(),
        }
    }
}

impl From<outcome_capnp::EventOutcome> for bridge::EventOutcome {
    fn from(value: outcome_capnp::EventOutcome) -> Self {
        match value {
            outcome_capnp::EventOutcome::Unknown => Self::Unknown,
            outcome_capnp::EventOutcome::Ok => Self::Ok,
            outcome_capnp::EventOutcome::Exception => Self::Exception,
            outcome_capnp::EventOutcome::ExceededCpu => Self::ExceededCpu,
            outcome_capnp::EventOutcome::KillSwitch => Self::KillSwitch,
            outcome_capnp::EventOutcome::DaemonDown => Self::DaemonDown,
            outcome_capnp::EventOutcome::ScriptNotFound => Self::ScriptNotFound,
            outcome_capnp::EventOutcome::Canceled => Self::Canceled,
            outcome_capnp::EventOutcome::ExceededMemory => Self::ExceededMemory,
            outcome_capnp::EventOutcome::LoadShed => Self::LoadShed,
            outcome_capnp::EventOutcome::ResponseStreamDisconnected => {
                Self::ResponseStreamDisconnected
            }
            outcome_capnp::EventOutcome::InternalError => Self::InternalError,
        }
    }
}
