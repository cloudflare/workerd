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
use outcome_capnp::EventOutcome;

use crate::AlarmResult;
use crate::CustomEvent;
use crate::CustomEventResult;
use crate::Interface;
use crate::Result;
use crate::ScheduledResult;
use crate::exception::SCRIPT_KILLED_DETAIL_ID;
use crate::ffi::Wrapper;

#[cxx::bridge(namespace = "workerd::rust::worker")]
pub mod bridge {
    extern "Rust" {
        type Wrapper = crate::ffi::Wrapper;

        #[expect(
            clippy::unnecessary_box_returns,
            reason = "c++ expects heap-allocation"
        )]
        fn new_kill_switch_worker() -> Box<Wrapper>;
    }
}

/// Worker implementation that reports Script Killed error for all methods.
pub struct Worker {}

impl Worker {
    fn error(file: &str, line: u32) -> Result<()> {
        Err(KjError::new(
            cxx::KjExceptionType::Overloaded,
            "jsg.Error: This script has been killed.".to_owned(),
        )
        .with_details(vec![(SCRIPT_KILLED_DETAIL_ID, vec![])])
        .with_location(file.to_owned(), line))
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
    ) -> Result<()> {
        Self::error(file!(), line!())
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Self::error(file!(), line!())
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Worker {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> Result<ScheduledResult> {
        Ok(ScheduledResult {
            retry: false,
            outcome: EventOutcome::KillSwitch,
        })
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> Result<AlarmResult> {
        Ok(AlarmResult {
            retry: false,
            retry_counts_against_limit: true,
            outcome: EventOutcome::KillSwitch,
            error_description: None,
        })
    }

    async fn custom_event(&mut self, _event: Pin<&mut CustomEvent>) -> Result<CustomEventResult> {
        Ok(CustomEventResult {
            outcome: EventOutcome::KillSwitch,
        })
    }
}

pub fn new_kill_switch_worker() -> Box<Wrapper> {
    Worker {}.into_ffi()
}
