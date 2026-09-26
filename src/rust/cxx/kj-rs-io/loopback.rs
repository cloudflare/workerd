//! `loopback:<name>` addresses: connections serviced within the process, for `workerd test`.
//!
//! A loopback address names a queue. `connect()` makes a socket pair ([`socket_pair`]), queues
//! one end and returns the other; a receiver's `accept()` takes from the queue. The sockets are
//! real, so a test exercises the whole stack above the transport. Loopback is off by default:
//! `parseAddress` reads `loopback:` as a hostname until the network's registry is enabled, which
//! workerd does for `workerd test` only -- in production, direct service bindings do the same
//! job with less machinery.
//!
//! `restrictPeers()` does not apply: it governs network peers, and a loopback connection has
//! none. The adapter (async-io.c++) lets loopback addresses through the filter.
//!
//! A name belongs to the loop that first used it: a queued connection is a socket registered
//! with the connector's runtime, so it must be accepted on that same runtime. Connecting or
//! accepting from another loop thread fails (`ensure_owner_loop`) before anything is queued,
//! rather than handing out a stream the receiver cannot use.

use std::collections::HashMap;
use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use tokio::sync::Notify;

use crate::current_loop_runtime_id;
use crate::ensure_owner_loop;
use crate::error::KjIoError;
use crate::error::Result;
use crate::net::socket_pair;
use crate::stream::TokioStream;

/// One `kj::Network`'s loopback namespace, shared with every network `restrictPeers()` derives
/// from it (like the peer filter chain). A handle to `Arc`-shared state.
pub struct LoopbackRegistry {
    shared: Arc<RegistryShared>,
}

struct RegistryShared {
    enabled: AtomicBool,
    queues: Mutex<HashMap<Vec<u8>, Arc<LoopbackQueue>>>,
}

/// The connections made to one loopback name and not yet accepted.
pub struct LoopbackQueue {
    name: Vec<u8>,
    /// The loop the name belongs to (module docs).
    owner: tokio::runtime::Id,
    pending: Mutex<VecDeque<Box<TokioStream>>>,
    /// Signalled once per push. A receiver registers its interest (`Notified::enable`) before
    /// it checks the queue, so a push between the check and the wait wakes it whatever the
    /// interleaving with other receivers.
    ready: Notify,
}

fn lock<'a, T>(mutex: &'a Mutex<T>, op: &'static str) -> Result<MutexGuard<'a, T>> {
    mutex
        .lock()
        .map_err(|_| KjIoError::other(op, "loopback state poisoned"))
}

impl LoopbackRegistry {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(RegistryShared {
                enabled: AtomicBool::new(false),
                queues: Mutex::new(HashMap::new()),
            }),
        }
    }

    /// Another handle to the same namespace.
    pub fn clone_handle(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
        }
    }

    /// Makes `parseAddress` accept `loopback:` addresses from now on. Cannot be reversed.
    pub fn enable(&self) {
        self.shared.enabled.store(true, Ordering::Release);
    }

    /// The queue a `loopback:<name>` address names, or `None` when `text` is not one. Until the
    /// registry is enabled nothing is, and `loopback:` reads as a hostname. The queue is created
    /// on first use and owned by the calling loop from then on.
    pub fn parse(&self, text: &[u8]) -> Option<Result<Arc<LoopbackQueue>>> {
        if !self.shared.enabled.load(Ordering::Acquire) {
            return None;
        }
        let name = text.strip_prefix(b"loopback:")?;
        Some(self.queue(name))
    }

    fn queue(&self, name: &[u8]) -> Result<Arc<LoopbackQueue>> {
        let owner = current_loop_runtime_id()?;
        let mut queues = lock(&self.shared.queues, "parseAddress")?;
        Ok(Arc::clone(queues.entry(name.to_vec()).or_insert_with(
            || Arc::new(LoopbackQueue::new(name.to_vec(), owner)),
        )))
    }
}

impl Default for LoopbackRegistry {
    fn default() -> Self {
        Self::new()
    }
}

impl LoopbackQueue {
    fn new(name: Vec<u8>, owner: tokio::runtime::Id) -> Self {
        Self {
            name,
            owner,
            pending: Mutex::new(VecDeque::new()),
            ready: Notify::new(),
        }
    }

    /// The name after `loopback:`.
    pub fn name(&self) -> &[u8] {
        &self.name
    }

    /// `connect()`: one end of a new socket pair is queued for the next `accept()`; the other is
    /// the caller's connection.
    pub fn connect(&self) -> Result<Box<TokioStream>> {
        ensure_owner_loop(self.owner)?;
        let (ours, theirs) = socket_pair()?;
        lock(&self.pending, "connect()")?.push_back(theirs);
        self.ready.notify_one();
        Ok(ours)
    }

    /// `accept()`: the next queued connection, waiting for one if the queue is empty. Several
    /// receivers may wait on one queue; each connection goes to exactly one of them.
    pub async fn accept(&self) -> Result<Box<TokioStream>> {
        ensure_owner_loop(self.owner)?;
        loop {
            let mut notified = std::pin::pin!(self.ready.notified());
            notified.as_mut().enable();
            // The guard is dropped before waiting: a `connect()` must be able to push meanwhile.
            let next = lock(&self.pending, "accept()")?.pop_front();
            if let Some(stream) = next {
                return Ok(stream);
            }
            notified.await;
        }
    }
}
