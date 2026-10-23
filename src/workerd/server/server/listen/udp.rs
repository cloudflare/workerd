// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! UDP sockets: the listener that serves each peer's datagrams as one flow, a `connect()` event
//! whose `workerd::DatagramChannel` is a [`UdpFlow`].

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::rc::Rc;
use std::time::Duration;

use futures::future::Either;
use kj_rs::KjMaybe;
use tokio::net::UdpSocket;
use worker::CxxWorkerInterface;
use worker::Interface;

use super::ListenContext;
use super::accept_loop;
use crate::Result;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::config::Factory;

/// The largest datagram the listener serves: comfortably above the largest UDP payload a peer
/// could ever send (65507 bytes plus headers). A larger one is truncated by the kernel with no way
/// to recover the tail, so it is dropped.
const MAX_DATAGRAM_SIZE: usize = 65535;

/// Receives one datagram into `buffer`, one byte longer than the largest datagram served: none for
/// a datagram that filled it, which the kernel cut short (Windows reports that as `WSAEMSGSIZE`).
async fn receive_datagram(
    socket: &UdpSocket,
    buffer: &mut [u8],
) -> std::io::Result<Option<(usize, SocketAddr)>> {
    match socket.recv_from(buffer).await {
        Ok((len, _)) if len == buffer.len() => Ok(None),
        Ok(received) => Ok(Some(received)),
        #[cfg(windows)]
        Err(e)
            if e.raw_os_error() == Some(windows_sys::Win32::Networking::WinSock::WSAEMSGSIZE) =>
        {
            Ok(None)
        }
        Err(e) => Err(e),
    }
}

/// The listener's side of a flow: where its datagrams go.
struct FlowInbox {
    id: u64,
    sender: tokio::sync::mpsc::UnboundedSender<Vec<u8>>,
    pending_bytes: Rc<Cell<usize>>,
    last_seen: Rc<Cell<Duration>>,
}

/// The flows in progress, by peer, for routing a later datagram to the flow already dispatched
/// for it.
type Flows = Rc<RefCell<HashMap<SocketAddr, FlowInbox>>>;

/// What a queued datagram counts against the flow's `maxPendingBytes`: its payload plus the
/// handle that holds it, so that a burst of tiny datagrams cannot queue without bound.
fn queued_size(datagram: &[u8]) -> usize {
    datagram.len() + std::mem::size_of::<Vec<u8>>()
}

/// One UDP flow: every datagram to and from one peer, until none has arrived for the idle
/// timeout. The `workerd::DatagramChannel` of the `connect()` event dispatched for the flow.
pub struct UdpFlow {
    id: u64,
    peer: SocketAddr,
    socket: Rc<UdpSocket>,
    receiver: RefCell<tokio::sync::mpsc::UnboundedReceiver<Vec<u8>>>,
    pending_bytes: Rc<Cell<usize>>,
    /// When the peer's last datagram arrived, on the factory's timer.
    last_seen: Rc<Cell<Duration>>,
    idle_timeout: Duration,
    factory: Rc<Factory>,
    ended: Cell<bool>,
    flows: Flows,
}

impl UdpFlow {
    /// The next datagram, or the end of the flow once it has been idle for the timeout.
    #[expect(
        clippy::await_holding_refcell_ref,
        reason = "the borrow is the one-call-at-a-time contract of DatagramChannel::receive()"
    )]
    pub async fn receive(&self) -> Result<ffi::UdpDatagram> {
        let ended = || ffi::UdpDatagram {
            ended: true,
            data: Vec::new(),
        };
        if self.ended.get() {
            return Ok(ended());
        }
        let mut receiver = self
            .receiver
            .try_borrow_mut()
            .map_err(|_| kj::failed!("DatagramChannel::receive() already has a pending call"))?;
        loop {
            let deadline = self.last_seen.get() + self.idle_timeout;
            let idle = deadline.saturating_sub(self.factory.now());
            let idle = std::pin::pin!(self.factory.sleep(idle));
            match futures::future::select(std::pin::pin!(receiver.recv()), idle).await {
                Either::Left((Some(data), _)) => {
                    self.pending_bytes
                        .set(self.pending_bytes.get().saturating_sub(queued_size(&data)));
                    return Ok(ffi::UdpDatagram { ended: false, data });
                }
                // A datagram that arrived meanwhile moved the deadline.
                Either::Right(_)
                    if self.factory.now() < self.last_seen.get() + self.idle_timeout => {}
                // The listener is gone, or the flow idled out.
                Either::Left((None, _)) | Either::Right(_) => break,
            }
        }
        self.ended.set(true);
        self.unregister();
        Ok(ended())
    }

    pub async fn send(&self, datagram: &[u8]) -> Result<()> {
        self.socket
            .send_to(datagram, self.peer)
            .await
            .map(drop)
            .map_err(|e| kj::failed!("UDP send failed: {e}"))
    }

    /// Stops routing the peer's datagrams here (unless a newer flow for the peer took over).
    fn unregister(&self) {
        let mut flows = self.flows.borrow_mut();
        if flows
            .get(&self.peer)
            .is_some_and(|inbox| inbox.id == self.id)
        {
            flows.remove(&self.peer);
        }
    }
}

impl Drop for UdpFlow {
    fn drop(&mut self) {
        self.unregister();
    }
}

/// Serves UDP on `socket`, each peer's datagrams as one flow.
///
/// A new peer's first datagram starts a flow, dispatched as one `connect()` event on the
/// service; later datagrams from the peer join it until it has been idle for `idle_timeout`. A
/// flow with `max_pending_bytes` queued drops what arrives next rather than holding up other
/// peers.
pub async fn listen_udp(
    context: Rc<ListenContext>,
    socket: UdpSocket,
    channel: Rc<dyn Channel>,
    address: String,
    idle_timeout: Duration,
    max_pending_bytes: usize,
) -> Result<()> {
    let socket = Rc::new(socket);
    let flows: Flows = Rc::default();
    let next_id = Cell::new(0u64);
    let address = Rc::new(address);
    let accept = || {
        let socket = Rc::clone(&socket);
        let flows = Rc::clone(&flows);
        let channel = Rc::clone(&channel);
        let address = Rc::clone(&address);
        let next_id = &next_id;
        let factory = &context.factory;
        async move {
            // One receive buffer per new flow; datagrams of known flows reuse it in the loop.
            let mut buffer = vec![0u8; MAX_DATAGRAM_SIZE + 1];
            loop {
                let Some((len, peer)) = receive_datagram(&socket, &mut buffer)
                    .await
                    .map_err(|e| kj::failed!("UDP receive failed: {e}"))?
                else {
                    continue;
                };
                let data = buffer[..len].to_vec();
                if let Some(inbox) = flows.borrow().get(&peer) {
                    inbox.last_seen.set(factory.now());
                    let queued = queued_size(&data);
                    // An empty queue takes any datagram, so one always reaches a waiting
                    // `receive()` however small `max_pending_bytes` is.
                    let pending = inbox.pending_bytes.get();
                    if (pending == 0 || pending + queued <= max_pending_bytes)
                        && inbox.sender.send(data).is_ok()
                    {
                        inbox.pending_bytes.set(pending + queued);
                    }
                    continue;
                }
                // A new peer: a new flow, dispatched as one connect() event.
                let id = next_id.get();
                next_id.set(id + 1);
                let (sender, receiver) = tokio::sync::mpsc::unbounded_channel();
                let pending_bytes = Rc::new(Cell::new(queued_size(&data)));
                let last_seen = Rc::new(Cell::new(factory.now()));
                let _ = sender.send(data);
                flows.borrow_mut().insert(
                    peer,
                    FlowInbox {
                        id,
                        sender,
                        pending_bytes: Rc::clone(&pending_bytes),
                        last_seen: Rc::clone(&last_seen),
                    },
                );
                let flow = Box::new(UdpFlow {
                    id,
                    peer,
                    socket: Rc::clone(&socket),
                    receiver: RefCell::new(receiver),
                    pending_bytes,
                    last_seen,
                    idle_timeout,
                    factory: Rc::clone(factory),
                    ended: Cell::new(false),
                    flows: Rc::clone(&flows),
                });
                return Ok(async move {
                    let metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
                    let mut worker = CxxWorkerInterface::new(channel.start_request(metadata)?);
                    let event = ffi::new_udp_connect_event(&address, flow);
                    worker.custom_event(event).await.map(drop)
                });
            }
        }
    };
    accept_loop(&context, "UDP connect() handler threw", accept, || {}).await
}

#[cfg(test)]
#[path = "udp-test.rs"]
mod tests;
