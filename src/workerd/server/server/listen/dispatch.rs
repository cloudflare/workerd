// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A loop thread's part of the sockets the front accepts from (`front/mod.rs`).
//!
//! The loop takes work from the front's queue and serves it on the socket's service, as a
//! listener of its own would serve what it accepted.
//!
//! An HTTP request goes through the handler a connection served on this loop has
//! (`HttpConnection`): the socket's rewriting, the peer's cf blob, the capnp session over
//! CONNECT. kj-hyper answers it over the way back the request carries, so the response is laid
//! out and a failed call answered as on one thread. A TCP connection is a `connect()` event.
//!
//! A loop takes an item when it has nothing else to run (`front::queue`): its event loop tells
//! the queue when the thread is about to sleep (`ParkHooks`), and the item it then takes gets
//! its first poll at once, in the turn that took it. The event loop also says when it goes on
//! to other work, since a wait can end before the task that takes has run: the queue then has
//! another loop take what this one was woken for. What the front's threads supply while the
//! loop serves an item, a request's body or a connection's bytes, the loop reads paced to its
//! own event port (`kj_rs_tokio::PortPace`), so that a client faster than the Worker that reads
//! it does not keep the loop from everything else.
//!
//! Each call in flight is a future of the one [`serve`] returns, so dropping that future, which
//! is how a loop that stops or fails ends, drops every call's KJ promise and gives up the
//! loop's place at the queue in the same step: what it has not taken stays queued for the other
//! loops. A call is also dropped when its connection drops it (the client went away), which is
//! how a Worker sees the abort.

use std::future::poll_fn;
use std::pin::pin;
use std::rc::Rc;
use std::task::Poll;

use futures::StreamExt;
use futures::future::LocalBoxFuture;
use futures::stream::FuturesUnordered;
use kj_hyper::server::ServerSettings;
use kj_rs_tokio::ParkHooks;

use super::HttpConnection;
use super::HttpSocket;
use super::ListenContext;
use super::TcpConnection;
use super::http_settings;
use super::log_connection_error;
use super::start_tcp;
use super::tcp_connection;
use crate::channels::Channel;
use crate::front::HttpWork;
use crate::front::TcpWork;
use crate::front::Work;
use crate::front::Worker;
use crate::front::queue::InFlight;
use crate::front::queue::Parking;
use crate::services::header_table;

/// What this loop serves on a socket the front accepts from.
pub enum Service {
    Http(Rc<HttpSocket>),
    /// Raw TCP: `connect()` events on the channel, addressed to the socket as bound.
    Tcp(Rc<dyn Channel>, Rc<str>),
}

/// Tells the queue when this thread sleeps, wakes, and goes on to other work.
struct Hooks(Parking<Work>);

impl ParkHooks for Hooks {
    fn before_park(&self) {
        self.0.before_park();
    }

    fn after_unpark(&self) {
        self.0.after_unpark();
    }

    fn before_resume(&self) {
        self.0.before_resume();
    }
}

/// Keeps a loop's hooks installed, and puts back what was there before.
struct Installed(Option<Rc<dyn ParkHooks>>);

impl Installed {
    fn new(parking: Parking<Work>) -> Self {
        Self(kj_rs_tokio::set_park_hooks(Some(Rc::new(Hooks(parking)))))
    }
}

impl Drop for Installed {
    fn drop(&mut self) {
        kj_rs_tokio::set_park_hooks(self.0.take());
    }
}

/// Serves what `worker` takes from the front's queue.
///
/// Resolves once the queue has ended and every call has finished. `services[i]` is what the
/// socket of index `i` serves; `None` for a socket the front does not accept from.
pub async fn serve(
    context: Rc<ListenContext>,
    worker: Worker,
    services: Vec<Option<Service>>,
) -> crate::Result<()> {
    let settings = Rc::new(http_settings());
    let _hooks = Installed::new(worker.parking());
    let mut calls: FuturesUnordered<LocalBoxFuture<'static, ()>> = FuturesUnordered::new();
    let mut ended = false;
    poll_fn(|cx| {
        while !ended {
            match worker.poll_take(cx) {
                // The call gets its first poll below, in this turn.
                Poll::Ready(Some((work, in_flight))) => {
                    calls.extend(start(&context, &settings, &services, work, in_flight));
                }
                Poll::Ready(None) => ended = true,
                Poll::Pending => break,
            }
        }
        while calls.poll_next_unpin(cx) == Poll::Ready(Some(())) {}
        if ended && calls.is_empty() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    })
    .await;
    Ok(())
}

/// The call that serves `work` on its socket's service. The front queues only what the sockets
/// it was given serve; work for anything else is dropped, which closes its connection.
fn start(
    context: &Rc<ListenContext>,
    settings: &Rc<ServerSettings>,
    services: &[Option<Service>],
    work: Work,
    in_flight: InFlight,
) -> Option<LocalBoxFuture<'static, ()>> {
    let context = Rc::clone(context);
    match work {
        Work::Http(work) => {
            let Some(Some(Service::Http(socket))) = services.get(work.socket) else {
                return None;
            };
            let (socket, settings) = (Rc::clone(socket), Rc::clone(settings));
            Some(Box::pin(http_call(
                context, socket, settings, work, in_flight,
            )))
        }
        Work::Tcp(work) => {
            let Some(Some(Service::Tcp(channel, authority))) = services.get(work.socket) else {
                return None;
            };
            let (channel, authority) = (Rc::clone(channel), Rc::clone(authority));
            Some(Box::pin(tcp_call(
                context, channel, authority, work, in_flight,
            )))
        }
    }
}

/// One HTTP request, until its call has finished or its connection has dropped it.
async fn http_call(
    context: Rc<ListenContext>,
    socket: Rc<HttpSocket>,
    settings: Rc<ServerSettings>,
    work: HttpWork,
    in_flight: InFlight,
) {
    let HttpWork {
        request,
        cf_blob,
        mut call,
        ..
    } = work;
    let handler = HttpConnection {
        context: Rc::clone(&context),
        socket,
        cf_blob: cf_blob.map(|blob| (*blob).to_owned()),
    };
    let table = header_table(&context.factory);
    {
        let handling = pin!(request.handle(table, &settings, &handler));
        // The connection dropped the call: the client is gone, or the front is. Dropping the
        // handler call drops its KJ promise, which is how the Worker's request is aborted.
        let dropped = pin!(call.closed());
        futures::future::select(handling, dropped).await;
    }
    drop(in_flight);
    // Tells the connection that the call is over.
    drop(call);
}

/// One TCP connection, until its `connect()` event has ended.
async fn tcp_call(
    context: Rc<ListenContext>,
    channel: Rc<dyn Channel>,
    authority: Rc<str>,
    work: TcpWork,
    in_flight: InFlight,
) {
    let serve = async {
        let worker = start_tcp(&*channel, work.peer)?;
        let connection = TcpConnection {
            io: work.io,
            hangup: work.hangup,
            starttls: work.starttls,
            of_front: true,
        };
        tcp_connection(&context, worker, &authority, connection).await
    };
    if let Err(error) = serve.await {
        log_connection_error("TCP connect() handler threw", &error);
    }
    drop(in_flight);
}
