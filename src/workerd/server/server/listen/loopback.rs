// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `loopback:<name>` addresses: connections serviced within the process, for `workerd test`.
//!
//! A loopback address names a queue. `connect()` makes an in-memory pipe (`tokio::io::duplex`),
//! queues one end and returns the other; the name's listener takes from the queue, so a
//! connection made before anyone listens waits there. Loopback is off by default: the addresses
//! are refused until the registry is enabled, which `workerd test` does -- in production, direct
//! service bindings do the same job with less machinery.
//!
//! Dialers run inside kj-hyper's client, which asks for `Send + Sync`: the registry is behind a
//! mutex, and the dialer of one peer holds a clone of it.

use std::cell::RefCell;
use std::collections::HashMap;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;

use kj_hyper::Hangup;
use kj_hyper::client::Dialed;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::DuplexStream;
use tokio::io::ReadBuf;
use tokio::sync::mpsc;
use tokio::sync::watch;

use crate::Result;

/// Each direction of a loopback connection buffers this much before the writer waits.
const BUFFER_SIZE: usize = 65536;

/// One end of a loopback connection. Dropping it resolves the other end's `hangup()`, as
/// dropping an end of kj's in-memory pipe resolves the other's `whenWriteDisconnected()`.
pub struct LoopbackStream {
    stream: DuplexStream,
    _alive: watch::Sender<()>,
    peer: watch::Receiver<()>,
}

impl LoopbackStream {
    fn pair() -> (Self, Self) {
        let (a, b) = tokio::io::duplex(BUFFER_SIZE);
        let (a_alive, a_gone) = watch::channel(());
        let (b_alive, b_gone) = watch::channel(());
        let end = |stream, alive, peer| Self {
            stream,
            _alive: alive,
            peer,
        };
        (end(a, a_alive, b_gone), end(b, b_alive, a_gone))
    }

    /// Resolves once the other end is dropped.
    pub fn hangup(&self) -> Hangup {
        let mut peer = self.peer.clone();
        Box::pin(async move {
            // Fails once the sender, which the other end holds, is gone.
            let _ = peer.changed().await;
            Ok(())
        })
    }
}

impl From<LoopbackStream> for Dialed {
    fn from(stream: LoopbackStream) -> Self {
        let hangup = stream.hangup();
        Self::with_hangup(stream, hangup)
    }
}

impl AsyncRead for LoopbackStream {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.stream).poll_read(cx, buf)
    }
}

impl AsyncWrite for LoopbackStream {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.stream).poll_write(cx, buf)
    }

    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.stream).poll_flush(cx)
    }

    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}

/// The server's loopback namespace: a handle to state shared by its clones.
#[derive(Clone, Default)]
pub struct Loopback {
    shared: Arc<Shared>,
}

#[derive(Default)]
struct Shared {
    enabled: AtomicBool,
    internet: AtomicBool,
    queues: Mutex<HashMap<String, Queue>>,
}

/// The connections made to one name and not yet accepted. The receiver is taken by the name's
/// listener.
struct Queue {
    sender: mpsc::UnboundedSender<LoopbackStream>,
    receiver: Option<mpsc::UnboundedReceiver<LoopbackStream>>,
}

/// The listening side of one name: there is at most one.
#[derive(Debug)]
pub struct LoopbackListener {
    name: String,
    receiver: RefCell<mpsc::UnboundedReceiver<LoopbackStream>>,
}

impl Loopback {
    /// Makes `loopback:` addresses usable from now on. Cannot be reversed.
    pub fn enable(&self) {
        self.shared.enabled.store(true, Ordering::Relaxed);
    }

    #[must_use]
    pub fn is_enabled(&self) -> bool {
        self.shared.enabled.load(Ordering::Relaxed)
    }

    /// Makes the `network` services connect within this registry instead: the name is the host,
    /// with `:port` unless the port is 80. For the server run inside a test process
    /// (`in_process.rs`), whose harness stands in for every host. Cannot be reversed.
    pub fn mock_internet(&self) {
        self.shared.internet.store(true, Ordering::Relaxed);
    }

    #[must_use]
    pub fn mocks_internet(&self) -> bool {
        self.shared.internet.load(Ordering::Relaxed)
    }

    /// `f` on the queue for `name`, created on first use.
    fn with_queue<T>(&self, name: &str, f: impl FnOnce(&mut Queue) -> T) -> Result<T> {
        if !self.is_enabled() {
            return Err(kj::failed!(
                "loopback: addresses are only available under `workerd test`: loopback:{name}"
            ));
        }
        let queues = &self.shared.queues;
        let mut queues = queues.lock().unwrap_or_else(PoisonError::into_inner);
        Ok(f(queues.entry(name.to_owned()).or_insert_with(|| {
            let (sender, receiver) = mpsc::unbounded_channel();
            Queue {
                sender,
                receiver: Some(receiver),
            }
        })))
    }

    /// The listener for `name`; a name is listened on once.
    pub fn listen(&self, name: &str) -> Result<LoopbackListener> {
        let receiver = self.with_queue(name, |queue| queue.receiver.take())?;
        receiver
            .map(|receiver| LoopbackListener {
                name: name.to_owned(),
                receiver: RefCell::new(receiver),
            })
            .ok_or_else(|| kj::failed!("loopback:{name} is already listened on"))
    }

    /// A connection to `name`: one end, the other queued for the name's listener. Fails once
    /// the listener is gone.
    pub fn connect(&self, name: &str) -> Result<LoopbackStream> {
        let (ours, theirs) = LoopbackStream::pair();
        self.with_queue(name, |queue| queue.sender.send(theirs))?
            .map_err(|_| {
                kj::disconnected!("loopback:{name}: the listener has stopped accepting connections")
            })?;
        Ok(ours)
    }
}

impl LoopbackListener {
    /// The next connection, waiting for one if none is queued.
    pub async fn accept(&self) -> Result<LoopbackStream> {
        // The borrow lasts one poll, never across a wait, so concurrent accepts cannot collide.
        std::future::poll_fn(|cx| self.receiver.borrow_mut().poll_recv(cx))
            .await
            .ok_or_else(|| kj::disconnected!("loopback:{}: the registry is gone", self.name))
    }
}

#[cfg(test)]
#[path = "loopback-test.rs"]
mod tests;
