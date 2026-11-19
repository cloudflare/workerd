// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The worker an alarm runs on when another instance owns its Durable Object.

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
use kj_rs::KjOwn;
use outcome_capnp::EventOutcome;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::Interface;
use worker::ScheduledResult;

/// What an alarm does when another instance owns its object: runs again later, a retry that
/// counts against no limit, or is done.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AlarmElsewhere {
    Retry,
    Done,
}

/// An alarm whose object another instance owns now: that instance runs it. The alarm itself
/// does nothing here, and every other event is unimplemented.
pub struct AlarmOwnedElsewhere {
    then: AlarmElsewhere,
}

impl AlarmOwnedElsewhere {
    #[must_use]
    pub fn new(then: AlarmElsewhere) -> Self {
        Self { then }
    }

    fn unimplemented() -> KjError {
        kj::not_implemented!("an alarm's interface only runs the alarm")
    }
}

#[async_trait::async_trait(?Send)]
impl kj::http::Service for AlarmOwnedElsewhere {
    async fn request<'a>(
        &'a mut self,
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _request_body: Pin<&'a mut AsyncInputStream>,
        _response: ServiceResponse<'a>,
    ) -> worker::Result<()> {
        Err(Self::unimplemented())
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> worker::Result<()> {
        Err(Self::unimplemented())
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for AlarmOwnedElsewhere {
    async fn prewarm(&mut self, _url: &str) -> worker::Result<()> {
        Err(Self::unimplemented())
    }

    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> worker::Result<ScheduledResult> {
        Err(Self::unimplemented())
    }

    async fn run_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
        _retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        let again = self.then == AlarmElsewhere::Retry;
        Ok(AlarmResult {
            retry: again,
            retry_counts_against_limit: false,
            outcome: if again {
                EventOutcome::Canceled
            } else {
                EventOutcome::Ok
            },
            error_description: None,
        })
    }

    // The owning instance holds the alarm state, so there is nothing to clear here.
    async fn abandon_alarm(
        &mut self,
        _scheduled_time: &SystemTime,
    ) -> worker::Result<Option<SystemTime>> {
        Ok(None)
    }

    async fn test(&mut self) -> worker::Result<bool> {
        Err(Self::unimplemented())
    }

    async fn custom_event(
        &mut self,
        _event: KjOwn<CustomEvent>,
    ) -> worker::Result<CustomEventResult> {
        Err(Self::unimplemented())
    }
}

#[cfg(test)]
#[path = "alarm_elsewhere-test.rs"]
mod tests;
