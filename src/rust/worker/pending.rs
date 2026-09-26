// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Pending`: an `Interface` whose target is still being started. Every event waits for the
//! target, then runs on it. Events on one `Pending` run one at a time, as on any `Interface`.

use std::pin::Pin;
use std::time::SystemTime;

use futures::future::LocalBoxFuture;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;

use crate::AlarmResult;
use crate::CustomEvent;
use crate::CustomEventResult;
use crate::CxxWorkerInterface;
use crate::Interface;
use crate::Result;
use crate::ScheduledResult;
use crate::ffi;
use crate::ffi::bridge;

enum State {
    Starting(LocalBoxFuture<'static, Result<KjOwn<bridge::WorkerInterface>>>),
    Started(CxxWorkerInterface),
    /// Starting failed; every event fails with the same error.
    Failed(cxx::KjError),
}

pub struct Pending {
    state: State,
}

impl Pending {
    /// Starts `target` once the first event arrives; that event and any later ones wait for it.
    pub fn new(
        target: impl Future<Output = Result<KjOwn<bridge::WorkerInterface>>> + 'static,
    ) -> Self {
        Self {
            state: State::Starting(Box::pin(target)),
        }
    }

    async fn target(&mut self) -> Result<&mut CxxWorkerInterface> {
        if let State::Starting(start) = &mut self.state {
            self.state = match start.await {
                Ok(target) => State::Started(CxxWorkerInterface::new(target)),
                Err(error) => State::Failed(error),
            };
        }
        match &mut self.state {
            State::Started(target) => Ok(target),
            State::Failed(error) => Err(error.clone()),
            State::Starting(_) => Err(kj::failed!("the worker did not start")),
        }
    }
}

#[async_trait::async_trait(?Send)]
impl Service for Pending {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let target = self.target().await?;
        target
            .request(method, url, headers, request_body, response)
            .await
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Result<()> {
        let target = self.target().await?;
        target
            .connect(host, headers, connection, response, settings)
            .await
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Pending {
    async fn prewarm(&mut self, url: &str) -> Result<()> {
        let target = self.target().await?;
        target.prewarm(url).await
    }

    async fn run_scheduled(
        &mut self,
        scheduled_time: &SystemTime,
        cron: &str,
    ) -> Result<ScheduledResult> {
        let target = self.target().await?;
        target.run_scheduled(scheduled_time, cron).await
    }

    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> Result<AlarmResult> {
        let target = self.target().await?;
        target.run_alarm(scheduled_time, retry_count).await
    }

    async fn abandon_alarm(&mut self, scheduled_time: &SystemTime) -> Result<Option<SystemTime>> {
        let target = self.target().await?;
        target.abandon_alarm(scheduled_time).await
    }

    async fn test(&mut self) -> Result<bool> {
        let target = self.target().await?;
        target.test().await
    }

    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        let target = match self.target().await {
            Ok(target) => target,
            Err(error) => {
                // The event learns why it is discarded: a JS RPC session resolves its client to
                // that error instead of reporting the event destroyed before completion.
                bridge::custom_event_failed(event, Box::new(ffi::Error::new(error.clone())));
                return Err(error);
            }
        };
        target.custom_event(event).await
    }
}
