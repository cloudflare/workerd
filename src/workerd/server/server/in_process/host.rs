// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The channels an [`InProcessServer`](super::InProcessServer) hands the program hosting it, to
//! the server's services and http sockets.
//!
//! The host's requests carry headers keyed by its own `kj::HttpHeaderTable`
//! (`WorkerFactory::Options::hostHeaderTable`), and the server's workers key theirs by the
//! factory's; a header id of one table must not be used with headers of the other. So headers
//! cross by name in both directions (the factory's `host_*` shims, bridge.rs).
//!
//! Closing the server revokes every channel handed out and every request started on one
//! ([`Revoker`]): what the host still holds, such as a JavaScript Fetcher, an RPC stub or an
//! unread response body, must not keep the closed server's factory alive. A request in progress
//! is canceled and fails with the revocation's reason, as do later ones.

use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::time::SystemTime;

use futures::future::Either;
use kj_rs::KjOwn;
use tokio::sync::Notify;
use worker::AlarmResult;
use worker::AsyncInputStream;
use worker::AsyncIoStream;
use worker::ConnectResponse;
use worker::ConnectSettings;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::CxxWorkerInterface;
use worker::HeadersRef;
use worker::Interface;
use worker::Method;
use worker::ScheduledResult;
use worker::Service;
use worker::ServiceResponse;

use crate::Result;
use crate::bridge;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::Factory;
use crate::services::not_transferable;
use crate::services::reborrow_settings;

/// The events a host channel delivers.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Events {
    /// Every event, as a service's channel does.
    All,
    /// HTTP requests (`fetch()`) only: an http socket takes nothing else.
    Http,
}

/// Revokes, with one reason, every channel it made and every request started on them.
#[derive(Default)]
pub struct Revoker(Rc<Revocation>);

impl Revoker {
    /// Revokes everything handed out, now and later. `reason` is what the host's failed requests
    /// say; the first reason stands.
    pub fn revoke(&self, reason: &str) {
        if self.0.is_revoked() {
            return;
        }
        *self.0.reason.borrow_mut() = Some(reason.to_owned());
        let held = self.0.held.take();
        for revocable in held.iter().filter_map(Weak::upgrade) {
            revocable.revoke();
        }
        self.0.revoked.notify_waiters();
    }

    /// Fails with the reason if revoked.
    pub fn check(&self) -> Result<()> {
        self.0.check()
    }

    /// A channel to `inner`, a channel of the server over `factory`, for the host to hold until
    /// revoked.
    pub fn channel(
        &self,
        inner: Rc<dyn Channel>,
        factory: &Rc<Factory>,
        events: Events,
    ) -> Result<Rc<dyn Channel>> {
        self.0.check()?;
        let channel = Rc::new(HostChannel {
            live: RefCell::new(Some(LiveChannel {
                inner,
                factory: Rc::clone(factory),
            })),
            revocation: Rc::clone(&self.0),
            events,
        });
        let held: Weak<HostChannel> = Rc::downgrade(&channel);
        self.0.hold(held);
        Ok(channel)
    }
}

#[derive(Default)]
struct Revocation {
    /// Why, once revoked.
    reason: RefCell<Option<String>>,
    /// Wakes the calls in progress when revoked.
    revoked: Notify,
    /// What revoking lets go of: the channels, and the requests between calls. Pruned of what is
    /// gone whenever something is added.
    held: RefCell<Vec<Weak<dyn Revocable>>>,
}

impl Revocation {
    fn is_revoked(&self) -> bool {
        self.reason.borrow().is_some()
    }

    /// The error of what fails because of the revocation.
    fn error(&self) -> crate::Error {
        match &*self.reason.borrow() {
            Some(reason) => kj::failed!("jsg.Error: {reason}"),
            None => kj::failed!("jsg.Error: The channel was revoked."),
        }
    }

    fn check(&self) -> Result<()> {
        if self.is_revoked() {
            Err(self.error())
        } else {
            Ok(())
        }
    }

    fn hold(&self, revocable: Weak<dyn Revocable>) {
        let mut held = self.held.borrow_mut();
        held.retain(|held| held.strong_count() > 0);
        held.push(revocable);
    }

    /// `call`, unless revoked first or meanwhile. Revoking drops `call`, which cancels it.
    async fn guard<T>(&self, call: impl Future<Output = Result<T>>) -> Result<T> {
        // Created before the check, so that it sees a revocation that comes after it.
        let revoked = self.revoked.notified();
        self.check()?;
        match futures::future::select(std::pin::pin!(call), std::pin::pin!(revoked)).await {
            Either::Left((result, _)) => result,
            Either::Right(((), _)) => Err(self.error()),
        }
    }
}

/// What lets go of the server when revoked.
trait Revocable {
    fn revoke(&self);
}

/// A channel the host holds.
struct HostChannel {
    /// The channel until revoked.
    live: RefCell<Option<LiveChannel>>,
    revocation: Rc<Revocation>,
    events: Events,
}

struct LiveChannel {
    inner: Rc<dyn Channel>,
    /// The factory the header shims use, kept alive while the channel can use it.
    factory: Rc<Factory>,
}

impl Revocable for HostChannel {
    fn revoke(&self) {
        drop(self.live.take());
    }
}

impl Channel for HostChannel {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let (inner, factory) = self
            .live
            .borrow()
            .as_ref()
            .map(|live| (Rc::clone(&live.inner), Rc::clone(&live.factory)))
            .ok_or_else(|| self.revocation.error())?;
        let slot = Rc::new(RequestSlot(RefCell::new(Some(LiveRequest {
            inner: CxxWorkerInterface::new(inner.start_request(metadata)?),
            factory,
        }))));
        let held: Weak<RequestSlot> = Rc::downgrade(&slot);
        self.revocation.hold(held);
        Ok(HostRequest {
            slot,
            revocation: Rc::clone(&self.revocation),
            events: self.events,
        }
        .into_kj())
    }

    fn require_allows_transfer(&self) -> Result<()> {
        Err(not_transferable("A channel to an in-process server"))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("A channel to an in-process server"))
    }
}

/// One request on a host channel.
struct HostRequest {
    slot: Rc<RequestSlot>,
    revocation: Rc<Revocation>,
    events: Events,
}

/// The request until revoked. A call takes it out of the slot for its duration, so revoking does
/// not drop it from under the call; the call, woken by the revocation, drops it.
struct RequestSlot(RefCell<Option<LiveRequest>>);

struct LiveRequest {
    inner: CxxWorkerInterface,
    /// The factory the header shims use, kept alive while the request can use it.
    factory: Rc<Factory>,
}

impl Revocable for RequestSlot {
    fn revoke(&self) {
        drop(self.0.take());
    }
}

impl HostRequest {
    /// The request, out of its slot for one call.
    fn take(&self) -> Result<LiveRequest> {
        self.revocation.check()?;
        self.slot
            .0
            .take()
            .ok_or_else(|| kj::failed!("a request on an in-process server ran two events at once"))
    }

    /// Puts the request back after a call, unless revoked meanwhile.
    fn put_back(&self, live: LiveRequest) {
        if !self.revocation.is_revoked() {
            *self.slot.0.borrow_mut() = Some(live);
        }
    }

    fn require_all_events(&self) -> Result<()> {
        match self.events {
            Events::All => Ok(()),
            Events::Http => Err(kj::failed!(
                "jsg.Error: A socket of an in-process server takes HTTP requests only."
            )),
        }
    }
}

/// Runs `$call`, an event on `$live` (the live request), unless the request is revoked first or
/// meanwhile.
macro_rules! guarded {
    ($self:ident, |$live:ident| $call:expr) => {{
        let mut $live = $self.take()?;
        let result = $self.revocation.guard($call).await;
        $self.put_back($live);
        result
    }};
}

#[async_trait::async_trait(?Send)]
impl Service for HostRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        guarded!(self, |live| async {
            let headers = ffi::host_request_headers(live.factory.raw(), headers.as_ffi())?;
            let mut response = bridge::host_response(live.factory.raw(), response.into_ffi())?;
            live.inner
                .request(
                    method,
                    url,
                    HeadersRef::from(&*headers),
                    request_body,
                    ServiceResponse::from(response.as_mut()),
                )
                .await
        })
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Result<()> {
        self.require_all_events()?;
        guarded!(self, |live| async {
            let headers = ffi::host_request_headers(live.factory.raw(), headers.as_ffi())?;
            let mut response =
                bridge::host_connect_response(live.factory.raw(), response.into_ffi())?;
            let mut tls_starter = settings.tls_starter.into();
            let settings = reborrow_settings(settings.use_tls, &mut tls_starter);
            live.inner
                .connect(
                    host,
                    HeadersRef::from(&*headers),
                    connection,
                    ConnectResponse::from(response.as_mut()),
                    settings,
                )
                .await
        })
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for HostRequest {
    async fn prewarm(&mut self, url: &str) -> Result<()> {
        guarded!(self, |live| live.inner.prewarm(url))
    }

    async fn run_scheduled(
        &mut self,
        scheduled_time: &SystemTime,
        cron: &str,
    ) -> Result<ScheduledResult> {
        self.require_all_events()?;
        guarded!(self, |live| live.inner.run_scheduled(scheduled_time, cron))
    }

    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> Result<AlarmResult> {
        self.require_all_events()?;
        guarded!(self, |live| live
            .inner
            .run_alarm(scheduled_time, retry_count))
    }

    async fn abandon_alarm(&mut self, scheduled_time: &SystemTime) -> Result<Option<SystemTime>> {
        self.require_all_events()?;
        guarded!(self, |live| live.inner.abandon_alarm(scheduled_time))
    }

    async fn test(&mut self) -> Result<bool> {
        self.require_all_events()?;
        guarded!(self, |live| live.inner.test())
    }

    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        match self.events {
            Events::All => guarded!(self, |live| live.inner.custom_event(event)),
            // An RPC call on a socket: what the event's type says when it is not supported.
            Events::Http => worker::not_supported(event).await,
        }
    }
}

#[cfg(test)]
#[path = "host-test.rs"]
mod tests;
