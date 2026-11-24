// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The interface a stub's request to a clustered Durable Object runs on.
//!
//! It tells the stub's [`MovedHook`] when the instance it reached answers that the object moved,
//! or the connection to that instance is lost.

use std::pin::Pin;
use std::time::SystemTime;

use cxx::KjException;
use cxx::KjExceptionType;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::CxxWorkerInterface;
use worker::Interface;
use worker::ScheduledResult;

use crate::actor::MovedHook;
use crate::bridge::ffi;
use crate::channels::WorkerInterface;

/// Where the interface a [`MovedWatcher`] forwards to runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Reach {
    /// On this instance.
    Local,
    /// On a peer, over its session.
    Peer,
}

/// What a failed event tells the [`MovedHook`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Seen {
    /// The instance answered that the object moved (`cluster::moved`).
    Moved,
    /// The peer, or the connection to it, failed.
    Lost,
}

/// Forwards every event to `inner`, and tells `hook` of each that fails with a `moved` answer.
///
/// On a peer, any other failure that neither side marked as before or after delivery is marked as
/// after it: the request may have run there, so it is not replayed. It stays retryable for the
/// application to decide. Such a failure that is a disconnect is the peer or the connection to it
/// failing, not the object, so `hook` is told of that too.
//
// TODO(perf): `RoutedChannel::start_request` wraps this in a `PromisedInterface` that resolves the
// object first. Look into one Rust interface that both resolves and watches, which saves the
// crossing into C++ and back between the two.
pub struct MovedWatcher {
    inner: CxxWorkerInterface,
    hook: MovedHook,
    reach: Reach,
}

impl MovedWatcher {
    /// `inner`, an interface on this instance, watched for `hook`, as a C++ `WorkerInterface`.
    pub fn local(inner: KjOwn<WorkerInterface>, hook: MovedHook) -> KjOwn<WorkerInterface> {
        Self::wrap(inner, hook, Reach::Local)
    }

    /// `inner`, an interface on a peer, watched for `hook`, as a C++ `WorkerInterface`.
    pub fn peer(inner: KjOwn<WorkerInterface>, hook: MovedHook) -> KjOwn<WorkerInterface> {
        Self::wrap(inner, hook, Reach::Peer)
    }

    fn wrap(
        inner: KjOwn<WorkerInterface>,
        hook: MovedHook,
        reach: Reach,
    ) -> KjOwn<WorkerInterface> {
        Self {
            inner: CxxWorkerInterface::new(inner),
            hook,
            reach,
        }
        .into_kj()
    }
}

/// `result`, the outcome of an event on an interface at `reach`, after telling `hook` what its
/// failure says. A free function, so that it can borrow the hook while `request()` and
/// `connect()` still borrow the inner interface for their lifetime.
fn watch<T>(hook: &MovedHook, reach: Reach, result: worker::Result<T>) -> worker::Result<T> {
    result.map_err(|mut error| {
        match classify(&mut error, reach) {
            Some(Seen::Moved) => hook.moved(),
            Some(Seen::Lost) => hook.lost(),
            None => {}
        }
        error
    })
}

/// What `error`, the failure of an event on an interface at `reach`, tells the hook. Marks a
/// peer's failure as after delivery if neither side marked it.
fn classify(error: &mut KjException, reach: Reach) -> Option<Seen> {
    let has = |id| error.detail(id).is_some();
    let not_delivered = has(ffi::request_not_delivered_detail_id());
    if not_delivered && has(ffi::actor_predecessor_rejected_detail_id()) {
        return Some(Seen::Moved);
    }
    if reach == Reach::Local || not_delivered || has(ffi::request_delivered_detail_id()) {
        return None;
    }
    error.set_detail(ffi::request_delivered_detail_id(), &[]);
    (error.r#type() == KjExceptionType::Disconnected).then_some(Seen::Lost)
}

#[async_trait::async_trait(?Send)]
impl Service for MovedWatcher {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> worker::Result<()> {
        let Self { inner, hook, reach } = self;
        let result = inner
            .request(method, url, headers, request_body, response)
            .await;
        watch(hook, *reach, result)
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> worker::Result<()> {
        let Self { inner, hook, reach } = self;
        let result = inner
            .connect(host, headers, connection, response, settings)
            .await;
        watch(hook, *reach, result)
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for MovedWatcher {
    async fn prewarm(&mut self, url: &str) -> worker::Result<()> {
        let result = self.inner.prewarm(url).await;
        watch(&self.hook, self.reach, result)
    }

    async fn run_scheduled(
        &mut self,
        scheduled_time: &SystemTime,
        cron: &str,
    ) -> worker::Result<ScheduledResult> {
        let result = self.inner.run_scheduled(scheduled_time, cron).await;
        watch(&self.hook, self.reach, result)
    }

    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        let result = self.inner.run_alarm(scheduled_time, retry_count).await;
        watch(&self.hook, self.reach, result)
    }

    async fn abandon_alarm(
        &mut self,
        scheduled_time: &SystemTime,
    ) -> worker::Result<Option<SystemTime>> {
        let result = self.inner.abandon_alarm(scheduled_time).await;
        watch(&self.hook, self.reach, result)
    }

    async fn test(&mut self) -> worker::Result<bool> {
        let result = self.inner.test().await;
        watch(&self.hook, self.reach, result)
    }

    async fn bench(&mut self, params: &[u64]) -> worker::Result<Vec<u64>> {
        let result = self.inner.bench(params).await;
        watch(&self.hook, self.reach, result)
    }

    async fn custom_event(
        &mut self,
        event: KjOwn<CustomEvent>,
    ) -> worker::Result<CustomEventResult> {
        let result = self.inner.custom_event(event).await;
        watch(&self.hook, self.reach, result)
    }
}

#[cfg(test)]
#[path = "moved-test.rs"]
mod tests;
