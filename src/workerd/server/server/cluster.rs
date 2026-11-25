// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Cluster mode (`Config.cluster`): this instance's membership in a cluster whose shared state
//! is a bucket (the `cluster` crate), and the port peers reach it on.
//!
//! The port speaks HTTP/1.1 through kj-hyper. `CONNECT`, its MAC in `cf-cluster-auth`, opens the
//! one capnp session an instance keeps to each peer: the tunnel carries `WorkerdDebugPort`,
//! whose `getActor` gives a `WorkerdBootstrap` for one Durable Object, so HTTP, WebSockets and
//! JS RPC to the object cross unchanged. A request for an object this instance
//! does not own routes on through its own channel to the owner. `GET /ready` answers whether
//! the instance takes traffic: it serves, holds its lease, and was not told to stop.
//!
//! With several loop threads (threads.rs) every thread has a [`ClusterNode`]. The first thread's
//! has the instance's control plane (`cluster::Cluster`: its record and lease, the one table of
//! what it claimed, the alarm index) and the peer port; every thread's has a data plane
//! (`cluster::Member`) for the objects whose home the thread is, with a bucket client and
//! sessions to peers of its own. A thread other than the first reaches the control plane
//! through its inbox ([`RemoteControl`]), which checks that the object is the asking thread's,
//! and reads the lease and the drain from what the threads share (`cluster::Shared`). A peer's
//! call arrives on the first thread, which counts it on the peer's session and passes it to the
//! object's home thread as a peer's.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::OnceLock;
use std::sync::PoisonError;
use std::time::Duration;

use cluster::Cluster;
use cluster::Control;
use cluster::Layout;
use cluster::Member;
use cluster::Route;
use cluster_logic::handoff::Budget;
use cluster_logic::handoff::GRACE_MS;
use cluster_logic::lease::Timing;
use futures::FutureExt;
use futures::TryFutureExt;
use futures::future::LocalBoxFuture;
use futures::future::Shared;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj_hyper::client::Dialed;
use kj_hyper::server::Connect;
use kj_hyper::server::Handler;
use kj_hyper::server::ServerSettings;
use kj_hyper::server::Shutdown;
use kj_rs::KjOwn;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpListener;
use tokio::net::TcpStream;
use tokio::sync::Notify;
use tokio::time::Instant;
use workerd_capnp::cluster_config;

use crate::Result;
use crate::actor::ActorNamespace;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::channels::attach;
use crate::config::Factory;
use crate::hub::ControlAnswer;
use crate::hub::ControlCall;
use crate::hub::Mail;
use crate::placement::Scope;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::send_error;

/// A session's CONNECT carries `<node>.<ms>.<nonce>.<mac>`: see `connect_mac`.
const AUTH_HEADER: &str = "cf-cluster-auth";
/// How far a CONNECT's timestamp may be from this instance's clock.
const AUTH_WINDOW_MS: u64 = 30_000;
/// How long dialing a peer and its answer to the CONNECT may take.
const TUNNEL_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

/// What the loop threads of an instance share of its membership, for a thread other than the
/// first to make its [`ClusterNode`] of.
#[derive(Clone)]
pub struct Seat {
    shared: cluster::Shared,
    /// The objects the instance hands off, named as a request's causes are
    /// (`ActorContainer::cause_name`), from when its drain begins.
    handed_off: Arc<Mutex<HashSet<String>>>,
    /// Each loop thread's count of its progress, for the lease thread to watch
    /// (`ClusterNode::ready`).
    progress: Arc<Mutex<Vec<cluster::Progress>>>,
    /// When the phases of the instance's stop end, set as its drain begins, before any thread
    /// sees that it drains.
    deadlines: Arc<OnceLock<Deadlines>>,
}

/// One loop thread's part in this instance's cluster: the crate's state and the factory sessions
/// to owners start from.
pub struct ClusterNode {
    /// This thread's data plane.
    pub cluster: Rc<Member>,
    /// The instance's control plane, on the first loop thread.
    first: Option<Rc<Cluster>>,
    seat: Seat,
    /// `unsafeTimeDivisor`: what the lease's times are divided by.
    pub time_divisor: u64,
    factory: Rc<Factory>,
    /// One session per peer instance at its address, dropped when it disconnects. A peer that
    /// vanishes without its session noticing keeps its entry for the life of the process.
    sessions: Rc<Sessions<Rc<RpcConnection>>>,
    /// The address peers dial, which their CONNECTs are bound to.
    advertise: String,
    /// The nonces of CONNECTs accepted within twice the window, by timestamp.
    nonces: RefCell<HashMap<String, u64>>,
    /// The clustered namespaces, by their part of the bucket's keys.
    namespaces: RefCell<HashMap<String, Weak<ActorNamespace>>>,
    /// How a stop's time is spent (`stopTimeoutMs`).
    budget: Budget,
    /// `ALARMS_INTERVAL`, unless a test shortens it.
    alarms_interval: Duration,
    /// The requests the debug port has in flight here; a peer's are its session's.
    peer_requests: Rc<Count>,
    /// The sessions peers have open here (`PeerConnection::connect`).
    peer_sessions: Rc<Count>,
    /// Set once this instance stops its peers' sessions (`stop_peer_sessions`): from then on it
    /// starts none of their requests, and accepts no session.
    refusing_peers: Rc<Signal>,
    /// Whether the application's listeners are served and the instance was not told to stop
    /// (`Ready`).
    ready: Cell<bool>,
    /// How long a stop waits from its drain's start to leave: the leases a service granted
    /// here (the cache's), else 0.
    pub leave_wait_ms: Cell<u64>,
    /// The `cache` services a peer's session reaches here (`ServerHandle::resolve_peer_cache`).
    pub caches: RefCell<HashSet<String>>,
    /// Whether this thread's event loop counts its progress for the lease thread (`watch_loop`).
    watched: Cell<bool>,
    /// Whether the lease thread watches the instance's event loops (`ready`).
    watching: Cell<bool>,
    /// How often the event loop counts its progress while it sleeps: once a lease round.
    beat: Duration,
}

/// When the phases of this instance's stop end (`cluster_logic::handoff::Budget`).
#[derive(Clone, Copy)]
pub struct Deadlines {
    pub in_flight: Instant,
    pub handoff: Instant,
    pub release: Instant,
    /// Leaving the cluster waits for this: a peer that lists this instance gone serves its cache
    /// keys at once, so the read leases it granted until it drained must have run out. Now,
    /// without a `cache` service.
    pub leave: Instant,
    pub grace: Instant,
    pub connections: Instant,
    pub stop: Instant,
}

/// A count, and its waiters for it to reach zero.
#[derive(Default)]
pub struct Count {
    count: Cell<usize>,
    none: Notify,
}

/// The instance takes traffic as far as its listeners go, until this is dropped: they are served,
/// and it was not told to stop.
pub struct Ready(Rc<ClusterNode>);

impl Drop for Ready {
    fn drop(&mut self) {
        self.0.ready.set(false);
    }
}

/// One of a `Count`, until it is dropped.
pub struct Counted(Rc<Count>);

impl Drop for Counted {
    fn drop(&mut self) {
        let count = self.0.count.get() - 1;
        self.0.count.set(count);
        if count == 0 {
            self.0.none.notify_waiters();
        }
    }
}

impl Count {
    /// Counts one until the guard is dropped.
    fn add(self: &Rc<Self>) -> Counted {
        self.count.set(self.count.get() + 1);
        Counted(Rc::clone(self))
    }

    /// Resolves once the count is zero.
    async fn none(&self) {
        loop {
            let none = self.none.notified();
            if self.count.get() == 0 {
                return;
            }
            none.await;
        }
    }
}

/// A flag that is set once, and its waiters for it.
#[derive(Default)]
struct Signal {
    set: Cell<bool>,
    notify: Notify,
}

impl Signal {
    fn is_set(&self) -> bool {
        self.set.get()
    }

    fn set(&self) {
        self.set.set(true);
        self.notify.notify_waiters();
    }

    async fn wait(&self) {
        loop {
            let set = self.notify.notified();
            if self.set.get() {
                return;
            }
            set.await;
        }
    }
}

/// How often an instance tidies its alarm index and adopts dead peers' alarms.
const ALARMS_INTERVAL: Duration = Duration::from_secs(5);

pub struct RpcConnection {
    client: KjOwn<ffi::RpcClient>,
    /// Set once the session it runs over disconnected: nothing sent on it arrives.
    down: Rc<Cell<bool>>,
}

type Dial<V> = LocalBoxFuture<'static, std::result::Result<V, String>>;

/// A peer instance's id and the address it is dialed at.
type Peer = (String, String);

/// One value per peer instance and its address, a session.
///
/// The callers that arrive while it is being made share the attempt, and a
/// failed attempt is forgotten, so the next caller makes another. Each
/// instance has a session of its own, also where instances share an address:
/// a session dialed to one passed its CONNECT, which another must pass for
/// itself.
pub struct Sessions<V> {
    map: RefCell<HashMap<Peer, Shared<Dial<V>>>>,
}

impl<V> Default for Sessions<V> {
    fn default() -> Self {
        Self {
            map: RefCell::default(),
        }
    }
}

impl<V: Clone + 'static> Sessions<V> {
    /// The value for instance `node` at `address`: the one made, the one being
    /// made, or one `make(node, address)` makes.
    pub async fn get(
        &self,
        node: &str,
        address: &str,
        make: impl FnOnce(&str, &str) -> Dial<V>,
    ) -> std::result::Result<V, String> {
        let key: Peer = (node.to_owned(), address.to_owned());
        let attempt = self.map.borrow().get(&key).cloned();
        let attempt = attempt.unwrap_or_else(|| {
            let attempt = make(node, address).shared();
            let mut map = self.map.borrow_mut();
            map.insert(key.clone(), attempt.clone());
            attempt
        });
        let result = attempt.clone().await;
        if result.is_err() {
            let mut map = self.map.borrow_mut();
            if map.get(&key).is_some_and(|a| a.ptr_eq(&attempt)) {
                map.remove(&key);
            }
        }
        result
    }

    /// Forgets the value for instance `node` at `address` if it is the one `is`
    /// matches.
    pub fn forget(&self, node: &str, address: &str, is: impl Fn(&V) -> bool) {
        let key: Peer = (node.to_owned(), address.to_owned());
        let mut map = self.map.borrow_mut();
        let made = map.get(&key).and_then(|a| a.peek());
        if made.is_some_and(|r| r.as_ref().is_ok_and(&is)) {
            map.remove(&key);
        }
    }
}

/// Refuses a public cluster or advertised address from the command line or config, or a wildcard
/// cluster address with no address to advertise.
///
/// These are the checks of `bind` that need no bound socket. `serve` makes them before it reads
/// the bucket, so it reports address errors without bucket access. `address` and `advertise` are
/// the values of `--cluster-addr` and `--cluster-advertise`.
pub fn check_addresses(
    conf: cluster_config::Reader<'_>,
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<()> {
    addresses(conf, address, advertise).map(drop)
}

/// The address to bind and, if one is given, the address to advertise, checked
/// (`check_addresses`).
fn addresses(
    conf: cluster_config::Reader<'_>,
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<(String, Option<String>)> {
    let address = address.map_or_else(|| text(conf.get_address()), |a| Ok(a.to_owned()))?;
    refuse_public(conf, &address)?;
    let advertise = match advertise {
        Some(advertise) => Some(advertise.to_owned()),
        None if conf.has_advertise() => Some(text(conf.get_advertise())?),
        None => None,
    };
    match &advertise {
        Some(advertise) => refuse_public(conf, advertise)?,
        None if is_wildcard(&address) => return Err(wildcard(&address)),
        None => {}
    }
    Ok((address, advertise))
}

/// Binds the cluster port, before the services start.
/// Returns the listener and the address to advertise.
pub async fn bind(
    conf: cluster_config::Reader<'_>,
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<(TcpListener, String)> {
    let (address, advertise) = addresses(conf, address, advertise)?;
    let listener = TcpListener::bind(&address)
        .await
        .map_err(|e| kj::failed!("cluster address {address}: {e}"))?;
    let bound = listener.local_addr().map_err(|e| kj::failed!("{e}"))?;
    let advertise = match advertise {
        Some(advertise) => with_bound_port(&advertise, bound.port()),
        // A host name that resolved to a wildcard.
        None if bound.ip().is_unspecified() => return Err(wildcard(&address)),
        None => {
            let advertise = bound.to_string();
            refuse_public(conf, &advertise)?;
            advertise
        }
    };
    Ok((listener, advertise))
}

/// `advertise` with a port of 0 replaced by `port`, the one bound; any other address as it is.
fn with_bound_port(advertise: &str, port: u16) -> String {
    match advertise.rsplit_once(':') {
        Some((host, "0")) => format!("{host}:{port}"),
        _ => advertise.to_owned(),
    }
}

/// Whether `address` is a wildcard IP address, `0.0.0.0` or `[::]` with a port.
fn is_wildcard(address: &str) -> bool {
    address
        .parse::<std::net::SocketAddr>()
        .is_ok_and(|a| a.ip().is_unspecified())
}

fn wildcard(address: &str) -> crate::Error {
    kj::failed!(
        "cluster address {address} is a wildcard, which peers cannot reach: set \
         cluster.advertise or --cluster-advertise"
    )
}

/// Fails for an `address` that is a public IP, unless the config allows it.
fn refuse_public(conf: cluster_config::Reader<'_>, address: &str) -> Result<()> {
    let Ok(ip) = address.parse::<std::net::SocketAddr>().map(|a| a.ip()) else {
        return Ok(());
    };
    if is_public(ip) && !conf.get_unsafe_public_address() {
        return Err(kj::failed!(
            "cluster address {ip} is public, and traffic between instances is not encrypted: \
             use a private network (a VPC, WireGuard, Tailscale), or set \
             cluster.unsafePublicAddress"
        ));
    }
    Ok(())
}

/// Whether `ip` is on a network others route: not loopback, private, link-local, shared
/// (100.64.0.0/10, which Tailscale uses), or a wildcard, which this cannot judge.
fn is_public(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(v4) => {
            let [a, b, ..] = v4.octets();
            let shared = a == 100 && (64..128).contains(&b);
            let local = v4.is_loopback() || v4.is_private() || v4.is_link_local();
            !(local || shared || v4.is_unspecified())
        }
        std::net::IpAddr::V6(v6) => {
            let local = v6.is_loopback() || v6.is_unique_local() || v6.is_unicast_link_local();
            !(local || v6.is_unspecified())
        }
    }
}

/// A new instance's id (`NodeRecord::node`): 16 random bytes in lowercase hex.
fn new_node_id() -> String {
    cluster::hex(&rand::random::<[u8; 16]>())
}

/// Whether `node` has the form of an instance's id (`new_node_id`).
fn is_node_id(node: &str) -> bool {
    node.len() == 32 && node.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// The cluster key's MAC of some bytes (`Cluster::mac`).
type Mac<'a> = dyn Fn(&[u8]) -> Vec<u8> + 'a;

/// The MAC a CONNECT to instance `node` at `target` carries, at `ms` with `nonce`: a header seen
/// on the wire is refused by any other instance, at any other target, once used, or once stale.
///
/// An address outlives its instance. A peer that dials a dead owner's address reaches whatever
/// took the port since: another instance of the cluster, or of another cluster with the same key
/// and a bucket of its own, which would serve the object from that bucket's state.
fn connect_mac(mac: &Mac<'_>, node: &str, target: &str, ms: u64, nonce: &str) -> Vec<u8> {
    mac(format!("workerd cluster connect v2\n{node}\n{target}\n{ms}\n{nonce}").as_bytes())
}

/// The `AUTH_HEADER` of a CONNECT to instance `node` at `target`. `node` comes from a record in
/// the bucket: one that is no instance id is refused, so that the signed text has one reading.
fn connect_auth(
    mac: &Mac<'_>,
    node: &str,
    target: &str,
    ms: u64,
    nonce: &str,
) -> std::io::Result<String> {
    if !is_node_id(node) {
        return Err(std::io::Error::other(format!(
            "a peer's record in the bucket names {node:?}, which is not an instance id"
        )));
    }
    let tag = cluster::hex(&connect_mac(mac, node, target, ms, nonce));
    Ok(format!("{node}.{ms}.{nonce}.{tag}"))
}

/// Checks a CONNECT's `AUTH_HEADER` at instance `me`, which peers dial at `advertise`: its MAC,
/// that it is for `me`, its clock against `now`, and that its nonce is not in `nonces`, which
/// then holds it.
fn check_connect_auth(
    mac: &Mac<'_>,
    me: &str,
    advertise: &str,
    header: Option<&[u8]>,
    now: u64,
    nonces: &mut HashMap<String, u64>,
) -> Result<()> {
    let bad = || kj::failed!("cluster CONNECT without a valid {AUTH_HEADER}");
    let header = header
        .and_then(|h| std::str::from_utf8(h).ok())
        .ok_or_else(bad)?;
    let [node, ms, nonce, tag] = header.split('.').collect::<Vec<_>>()[..] else {
        return Err(bad());
    };
    if !is_node_id(node) {
        return Err(bad());
    }
    let ms: u64 = ms.parse().map_err(|_| bad())?;
    let tag = cluster::unhex(tag).ok_or_else(bad)?;
    if !constant_time_eq(&connect_mac(mac, node, advertise, ms, nonce), &tag) {
        return Err(bad());
    }
    if node != me {
        return Err(kj::failed!(
            "cluster CONNECT for instance {node}, which this one ({me}) is not"
        ));
    }
    if now.abs_diff(ms) > AUTH_WINDOW_MS {
        return Err(kj::failed!(
            "cluster CONNECT {}ms off this clock",
            now.abs_diff(ms)
        ));
    }
    nonces.retain(|_, t| now.abs_diff(*t) <= 2 * AUTH_WINDOW_MS);
    if nonces.insert(nonce.to_owned(), ms).is_some() {
        return Err(kj::failed!("cluster CONNECT replayed"));
    }
    Ok(())
}

/// How long a fenced instance waits for the line that says why before it exits, at most. A lease
/// fences seconds before a peer may take over (a quarter of that with a test's
/// `unsafeTimeDivisor` of 4).
const FENCE_LOG_WAIT: Duration = Duration::from_millis(200);
/// What a fenced instance keeps of the time its lease still holds, for its exit to be late by.
const FENCE_SLACK: Duration = Duration::from_millis(20);

/// How long a fenced instance whose lease holds for `remaining` more may wait for the line that
/// says why before it exits: `None` if it must exit at once, with no more of that line than
/// stderr takes without blocking.
fn fence_log_wait(remaining: Duration) -> Option<Duration> {
    let wait = remaining.checked_sub(FENCE_SLACK)?.min(FENCE_LOG_WAIT);
    (!wait.is_zero()).then_some(wait)
}

impl ClusterNode {
    /// Joins the cluster: the crate's state for `conf`, this instance at `address`.
    pub fn new(
        factory: Rc<Factory>,
        conf: cluster_config::Reader<'_>,
        address: String,
    ) -> Result<Rc<Self>> {
        let options = conf.get_bucket_options().map_err(|e| capnp(&e))?;
        let divisor = conf.get_unsafe_time_divisor();
        let Some(timing) = Timing::divided(divisor.into()) else {
            return Err(kj::failed!(
                "cluster: unsafeTimeDivisor {divisor} rounds a lease time down to 0 ms"
            ));
        };
        let setup = cluster::Setup {
            url: text(conf.get_bucket())?,
            options: options.iter().map(text).collect::<Result<_>>()?,
            node: new_node_id(),
            address: address.clone(),
            timing,
            key: text(conf.get_key())?.into_bytes(),
        };
        // Called on the lease thread, whatever the loop thread is doing. Peers may take this
        // instance's objects over once its lease lapses, so it stops serving first: at once,
        // without the destructors that would wait for the loop. The reason's line may block (on
        // a stalled pipe, or a lock of the logger's), so the exit is armed before it is written,
        // for while the lease still holds. With no time for that, the line goes to stderr as far
        // as a write that does not block takes it.
        let fence = Box::new(|reason: String, remaining: Duration| {
            let message = format!("leaving the cluster: {reason}");
            let Some(wait) = fence_log_wait(remaining) else {
                crate::log::fatal_nonblocking(&message);
                ffi::cli_exit(1);
                return;
            };
            let armed = std::thread::Builder::new()
                .name("cluster-fence".into())
                .spawn(move || {
                    std::thread::sleep(wait);
                    ffi::cli_exit(1);
                });
            if armed.is_ok() {
                crate::log::fatal(&message);
            } else {
                crate::log::fatal_nonblocking(&message);
            }
            ffi::cli_exit(1);
        });
        let cluster = Cluster::new(setup, fence, crate::log::within_thread())
            .map_err(|e| kj::failed!("cluster: {e}"))?;
        let seat = Seat {
            shared: cluster.shared().clone(),
            handed_off: Arc::default(),
            progress: Arc::default(),
            deadlines: Arc::default(),
        };
        Ok(Rc::new(Self {
            cluster: Rc::clone(cluster.member()),
            first: Some(cluster),
            seat,
            time_divisor: divisor.into(),
            factory,
            sessions: Rc::default(),
            advertise: address,
            nonces: RefCell::default(),
            namespaces: RefCell::default(),
            budget: Budget {
                stop_ms: conf.get_stop_timeout_ms().into(),
                accept_ms: GRACE_MS / u64::from(divisor),
            },
            alarms_interval: ALARMS_INTERVAL / divisor,
            peer_requests: Rc::default(),
            peer_sessions: Rc::default(),
            refusing_peers: Rc::default(),
            ready: Cell::new(false),
            leave_wait_ms: Cell::new(0),
            caches: RefCell::default(),
            watched: Cell::new(false),
            watching: Cell::new(false),
            beat: Duration::from_millis(timing.interval_ms),
        }))
    }

    /// This thread's part in the cluster of the instance whose first loop thread made `seat`:
    /// a data plane for the objects whose home this thread is, over a bucket client of its own.
    pub fn member(
        factory: Rc<Factory>,
        conf: cluster_config::Reader<'_>,
        seat: Seat,
    ) -> Result<Rc<Self>> {
        let divisor = conf.get_unsafe_time_divisor();
        let Some(timing) = Timing::divided(divisor.into()) else {
            return Err(kj::failed!(
                "cluster: unsafeTimeDivisor {divisor} rounds a lease time down to 0 ms"
            ));
        };
        let bucket = (seat.shared.open_bucket()).map_err(|e| kj::failed!("cluster: {e}"))?;
        let control = Rc::new(RemoteControl {
            factory: Rc::clone(&factory),
            bucket: bucket.clone(),
        });
        let member = Member::new(bucket, seat.shared.clone(), control);
        Ok(Rc::new(Self {
            cluster: Rc::new(member),
            first: None,
            seat,
            time_divisor: divisor.into(),
            factory,
            sessions: Rc::default(),
            advertise: String::new(),
            nonces: RefCell::default(),
            namespaces: RefCell::default(),
            budget: Budget {
                stop_ms: conf.get_stop_timeout_ms().into(),
                accept_ms: GRACE_MS / u64::from(divisor),
            },
            alarms_interval: ALARMS_INTERVAL / divisor,
            peer_requests: Rc::default(),
            peer_sessions: Rc::default(),
            refusing_peers: Rc::default(),
            ready: Cell::new(false),
            leave_wait_ms: Cell::new(0),
            caches: RefCell::default(),
            watched: Cell::new(false),
            watching: Cell::new(false),
            beat: Duration::from_millis(timing.interval_ms),
        }))
    }

    /// What a thread other than the first makes its node of (`member`).
    #[must_use]
    pub fn seat(&self) -> Seat {
        self.seat.clone()
    }

    /// The instance's control plane, which the first loop thread has.
    pub fn first(&self) -> Result<&Rc<Cluster>> {
        self.first
            .as_ref()
            .ok_or_else(|| kj::failed!("the cluster's control plane is the first loop thread's"))
    }

    /// Has the lease thread watch this thread's event loop too, from when the first thread
    /// starts the watch (`ready`). The loop beats once a round while it sleeps. A loop that
    /// cannot be watched serves on, and says that it does.
    pub fn watch_loop(&self) {
        if self.watched.replace(true) {
            return;
        }
        match kj_rs_tokio::watch_progress(self.beat) {
            Ok(progress) => {
                let mut watched = self.seat.progress.lock();
                let watched = watched.as_mut().unwrap_or_else(|e| e.get_mut());
                watched.push(Box::new(move || progress.count()));
            }
            Err(e) => tracing::warn!(
                "cluster: the lease thread cannot watch an event loop, so a loop that makes no \
                 progress will not fence this node: {e}"
            ),
        }
    }

    /// Marks the instance as taking traffic, as far as its listeners go, until the guard is
    /// dropped. On the first loop thread, once every loop thread is ready.
    ///
    /// The first time, the lease thread starts watching the event loops of the instance's
    /// threads, each of which owns objects (`Cluster::watch`): the count it reads advances only
    /// when every loop has made progress since it last did, so one loop that stands still
    /// fences the node as the only loop of an instance does. Not before: linking runs in turns
    /// as long as itself.
    pub fn ready(self: &Rc<Self>) -> Ready {
        self.watch_loop();
        if let Some(first) = &self.first
            && !self.watching.replace(true)
        {
            let loops = std::mem::take(
                &mut *(self.seat.progress.lock()).unwrap_or_else(PoisonError::into_inner),
            );
            if !loops.is_empty() {
                first.watch(every_loop(loops));
            }
        }
        self.ready.set(true);
        Ready(Rc::clone(self))
    }

    /// Starts draining: this instance claims nothing more, and its stop's deadlines run from now.
    /// On the first loop thread.
    pub fn start_draining(&self) -> Result<Deadlines> {
        let first = Rc::clone(self.first()?);
        let now = Instant::now();
        let at = |ms| now + Duration::from_millis(ms);
        let deadlines = Deadlines {
            in_flight: at(self.budget.in_flight_ms()),
            handoff: at(self.budget.handoff_ms()),
            release: at(self.budget.release_ms()),
            leave: at(self.leave_wait_ms.get()),
            grace: at(self.budget.grace_ms()),
            connections: at(self.budget.connections_ms()),
            stop: at(self.budget.stop_ms),
        };
        // Every thread has the deadlines before it sees that the instance drains.
        let _ = self.seat.deadlines.set(deadlines);
        first.start_draining();
        self.factory
            .spawn_detached(async move { first.announce_draining().await });
        Ok(deadlines)
    }

    /// When the phases of the instance's stop end, once it drains.
    fn deadlines(&self) -> Option<Deadlines> {
        self.seat.deadlines.get().copied()
    }

    /// Hands off an object this draining instance owns at `epoch` but does not run, once it is
    /// at rest here, unless the handoff's deadline passes first. Answers whether it was released;
    /// one that was not is abandoned (`Cluster::abandon_actor`).
    pub async fn release(&self, namespace: &str, actor: &str, epoch: u64) -> bool {
        let Some(deadlines) = self.deadlines() else {
            return false;
        };
        let release = self.cluster.release_actor(namespace, actor, epoch);
        let released = match tokio::time::timeout_at(deadlines.handoff, release).await {
            Ok(Ok(released)) => released,
            Ok(Err(e)) => {
                tracing::warn!("cluster: handing off {namespace}/{actor}: {e}");
                false
            }
            Err(_) => false,
        };
        if !released {
            self.cluster.abandon_actor(namespace, actor);
        }
        released
    }

    /// When a relay of a peer's request that waited here for its object's handoff must start by,
    /// once this instance stops; none before. Sent on, the request does not outlast its caller's
    /// retries; but a call this instance relays when its connections deadline comes is cut off as
    /// one that may have run, which the peer cannot send again. So it relays only while the call
    /// has the time work in flight on an object being handed off has had, until the `in_flight`
    /// deadline; from then on it answers `moved`, and the peer sends the request where the object
    /// went itself.
    pub fn handed_off_relay_deadline(&self) -> Option<Instant> {
        self.deadlines().map(|deadlines| deadlines.in_flight)
    }

    /// Starts a request a peer makes here, or the debug port: counted on the peer's `session`,
    /// or else on this instance, until the guard is dropped. Once this instance stops its peers'
    /// sessions it answers a peer's `moved` instead, before anything runs, and the peer sends the
    /// request where the object runs now. The debug port's requests are served as before.
    pub fn peer_request(&self, session: Option<&Rc<Count>>) -> Result<Counted> {
        match session {
            Some(_) if self.refusing_peers.is_set() => Err(moved("this instance is stopping")),
            Some(session) => Ok(session.add()),
            None => Ok(self.peer_requests.add()),
        }
    }

    /// Ends its peers' sessions, once it serves nothing else. It starts none of their requests
    /// from now on, and closes each session once the requests it started on it have ended: the
    /// peer reads `moved` as its reason, which fails every call it still has out on the session,
    /// none of which started here, as `moved` too, so it sends them again where the objects run
    /// now. Resolves once every session has closed, and the debug port's requests have ended.
    pub async fn stop_peer_sessions(&self) {
        self.refusing_peers.set();
        futures::join!(self.peer_requests.none(), self.peer_sessions.none());
    }

    pub fn add_namespace(&self, unique_key: &str, namespace: Weak<ActorNamespace>) {
        let hash = cluster::namespace_hash(unique_key);
        self.namespaces.borrow_mut().insert(hash, namespace);
    }

    /// Whether this instance is handing off `object`, named `<namespace hash>/<root key>`, on
    /// whichever loop thread the object lives.
    pub fn hands_off(&self, object: &str) -> bool {
        let handed_off = self.seat.handed_off.lock();
        handed_off
            .unwrap_or_else(PoisonError::into_inner)
            .contains(object)
    }

    /// This thread hands `object` off, named `<namespace hash>/<root key>`: every thread admits
    /// what its work makes from now on (`hands_off`).
    pub fn handing_off(&self, object: String) {
        let handed_off = self.seat.handed_off.lock();
        handed_off
            .unwrap_or_else(PoisonError::into_inner)
            .insert(object);
    }

    /// Schedules the alarm, adopted from a dead peer, of the object `actor` of the namespace
    /// whose hash is `namespace`: here if its home is this thread, else on its home thread.
    pub fn adopt_alarm(&self, namespace: &str, actor: &str, name: Option<&str>, at_ms: u64) {
        let found = self.namespaces.borrow().get(namespace).cloned();
        let Some(found) = found.and_then(|ns| ns.upgrade()) else {
            // Not configured here: a peer that has it adopts it once this one dies.
            tracing::warn!("an adopted alarm's namespace is not configured here");
            return;
        };
        let home = found.home(actor);
        if home != self.factory.thread() {
            let mail = Mail::AdoptAlarm {
                namespace: namespace.to_owned(),
                actor: actor.to_owned(),
                name: name.map(str::to_owned),
                at_ms,
            };
            if !self.factory.post(home, mail) {
                tracing::warn!("an adopted alarm's loop thread has ended");
            }
            return;
        }
        if let Err(e) = found.adopt_alarm(actor, name, at_ms) {
            tracing::warn!("scheduling an adopted alarm: {}", e.description());
        }
    }

    /// Keeps this instance's alarm index tidy and schedules its share of dead peers' alarms,
    /// each on its object's home thread. On the first loop thread. Never returns.
    pub async fn alarms_loop(&self) {
        let Ok(first) = self.first().map(Rc::clone) else {
            return;
        };
        loop {
            tokio::time::sleep(self.alarms_interval).await;
            // A draining instance takes on no alarms; its own go to its peers as it leaves.
            if self.cluster.is_draining() {
                continue;
            }
            let schedule = |alarm: cluster::Adopted| {
                self.adopt_alarm(
                    &alarm.namespace,
                    &alarm.actor,
                    alarm.name.as_deref(),
                    alarm.at_ms,
                );
            };
            first.alarms_round(cluster::now_ms(), schedule).await;
        }
    }

    /// What loop thread `from` asked of the control plane, which this thread, the first, has.
    /// A thread asks only for the objects whose home it is: the one table of what the instance
    /// owns is changed for an object by one thread alone.
    pub async fn control(&self, from: usize, call: ControlCall) -> ControlAnswer {
        let refused = |call: &ControlCall| {
            let (ns, actor) = call.object();
            let home = self.factory.home(&Scope::Durable(ns), actor);
            (home != from).then(|| {
                tracing::error!(
                    "loop thread {from} asked the cluster's control plane for {ns}/{actor}, \
                     whose home is thread {home}"
                );
                cluster::Error::Replica(format!(
                    "{ns}/{actor}: asked for by a loop thread that is not its home"
                ))
            })
        };
        let first = match (self.first(), refused(&call)) {
            (Ok(first), None) => Rc::clone(first),
            (Err(e), _) => return call.failed(cluster::Error::Replica(e.description().to_owned())),
            (_, Some(e)) => return call.failed(e),
        };
        let control = first.control();
        match call {
            ControlCall::Claim { ns, actor, now_ms } => {
                ControlAnswer::Claim(control.claim(&ns, &actor, now_ms).await)
            }
            ControlCall::Advance { ns, actor, epoch } => {
                ControlAnswer::Advance(control.advance(&ns, &actor, epoch).await)
            }
            ControlCall::Open { ns, actor, epoch } => {
                ControlAnswer::Flag(control.open(&ns, &actor, epoch).await)
            }
            ControlCall::Release { ns, actor, epoch } => {
                ControlAnswer::Flag(control.release(&ns, &actor, epoch).await)
            }
            ControlCall::Abandon { ns, actor } => {
                control.abandon(&ns, &actor);
                ControlAnswer::Done(Ok(()))
            }
            ControlCall::ForgetRoute { ns, actor, node } => {
                control.forget_route(&ns, &actor, &node);
                ControlAnswer::Done(Ok(()))
            }
            ControlCall::RefreshRoute { ns, actor, now_ms } => {
                ControlAnswer::Done(control.refresh_route(&ns, &actor, now_ms).await)
            }
            ControlCall::ArmAlarm {
                ns,
                actor,
                name,
                at_ms,
            } => ControlAnswer::Done(control.arm_alarm(&ns, &actor, name.as_deref(), at_ms).await),
            ControlCall::OpenedAlarm {
                ns,
                actor,
                name,
                at_ms,
            } => ControlAnswer::Done(
                control
                    .opened_alarm(&ns, &actor, name.as_deref(), at_ms)
                    .await,
            ),
            ControlCall::AlarmElsewhere {
                ns,
                actor,
                owner,
                now_ms,
            } => ControlAnswer::Flag(control.alarm_elsewhere(&ns, &actor, &owner, now_ms).await),
        }
    }

    /// Where a request for Durable Object `actor` of `namespace` should run.
    /// If this instance owns the object, or claims it now, the result is
    /// `Owner::Here` with the ownership epoch. Otherwise it is
    /// `Owner::Remote`: a stub, over the session to the instance that owns
    /// the object, for the object as class `class` of worker `service`.
    pub async fn route(
        &self,
        namespace: &str,
        service: &str,
        class: &str,
        actor: &str,
    ) -> Result<Owner> {
        let now = cluster::now_ms();
        let route = self
            .cluster
            .acquire_or_route(namespace, actor, now)
            .await
            .map_err(|e| match e {
                cluster::Error::NotHere(_) => moved(&format!("{namespace}/{actor}: {e}")),
                e => kj::disconnected!("{namespace}/{actor}: {e}"),
            })?;
        let (node, address, stale) = match route {
            Route::Owned(epoch) => return Ok(Owner::Here(epoch)),
            Route::Releasing(epoch) => return Ok(Owner::Releasing(epoch)),
            Route::Remote {
                node,
                address,
                stale,
            } => (node, address, stale),
        };
        if stale {
            let cluster = Rc::clone(&self.cluster);
            let (namespace, actor) = (namespace.to_owned(), actor.to_owned());
            self.factory.spawn_detached(async move {
                let _ = cluster.refresh_route(&namespace, &actor, now).await;
            });
        }
        let session = self.session(&node, &address).await.map_err(|e| {
            // Unreachable: its own record, not the listing, tells whether it still owns the
            // object (`forget_route`).
            self.cluster.forget_route(namespace, actor, &node);
            kj::disconnected!("{namespace}/{actor} on {node}: {e}")
        })?;
        // A channel per stub, so a stub stays with the instance it reached.
        let connection = RpcConnection {
            client: ffi::rpc_client_debug_actor(
                &session.client,
                service,
                class,
                actor,
                kj_rs::KjMaybe::None,
                crate::actor::Caller::Stub.code(),
            )?,
            down: Rc::clone(&session.down),
        };
        Ok(Owner::Remote(Rc::new(connection), node))
    }

    /// Service `service` on node `node`, over this instance's session to it: an HTTP channel to
    /// what that node's config names so (`WorkerdDebugPort.getEntrypoint`). Fails at once for a
    /// node the listing does not have live, and for one that cannot be reached; its requests
    /// fail as soon as the session does.
    pub async fn peer_service(&self, node: &str, service: &str) -> Result<Rc<dyn Channel>> {
        let address = self
            .first()?
            .address(node, cluster::now_ms())
            .ok_or_else(|| kj::disconnected!("cluster node {node} is not live"))?;
        let session = self
            .session(node, &address)
            .await
            .map_err(|e| kj::disconnected!("cluster node {node}: {e}"))?;
        Ok(Rc::new(PeerService(Rc::new(RpcConnection {
            client: ffi::rpc_client_debug_entrypoint(&session.client, service)?,
            down: Rc::clone(&session.down),
        }))))
    }

    /// The session to instance `node` at `address`: the open one, the one
    /// being dialed, or a new one.
    async fn session(&self, node: &str, address: &str) -> std::io::Result<Rc<RpcConnection>> {
        let dial = |node: &str, address: &str| self.dial(node, address);
        let session = self.sessions.get(node, address, dial).await;
        session.map_err(std::io::Error::other)
    }

    fn dial(&self, node: &str, address: &str) -> Dial<Rc<RpcConnection>> {
        let cluster = Rc::clone(&self.cluster);
        let factory = Rc::clone(&self.factory);
        let sessions = Rc::clone(&self.sessions);
        let (node, address) = (node.to_owned(), address.to_owned());
        async move {
            // A stalled peer accepts the connection and never answers.
            let tunnel = tokio::time::timeout(TUNNEL_TIMEOUT, tunnel(&cluster, &node, &address));
            let stream = tunnel
                .await
                .map_err(|_| "no answer to CONNECT".to_owned())?
                .map_err(|e| e.to_string())?;
            let session = Rc::new(RpcConnection {
                client: ffi::new_rpc_client(factory.raw(), Dialed::from(stream).into_kj()),
                down: Rc::default(),
            });
            let watched = Rc::clone(&session);
            factory.spawn_detached(async move {
                let _ = ffi::rpc_client_on_disconnect(&watched.client).await;
                watched.down.set(true);
                sessions.forget(&node, &address, |s| Rc::ptr_eq(s, &watched));
            });
            Ok(session)
        }
        .boxed_local()
    }
}

/// A tunnel to instance `node` at `address`, refused by anything else there.
async fn tunnel(cluster: &Member, node: &str, address: &str) -> std::io::Result<TcpStream> {
    let mut stream = TcpStream::connect(address).await?;
    stream.set_nodelay(true)?;
    let ms = cluster::now_ms();
    let nonce = cluster::hex(&rand::random::<[u8; 16]>());
    let auth = connect_auth(&|data| cluster.mac(data), node, address, ms, &nonce)?;
    let auth = format!("{AUTH_HEADER}: {auth}");
    let request = format!("CONNECT cluster:1 HTTP/1.1\r\nhost: cluster:1\r\n{auth}\r\n\r\n");
    stream.write_all(request.as_bytes()).await?;
    // The response head, read a byte at a time so that nothing past it is consumed.
    let mut head = Vec::new();
    while !head.ends_with(b"\r\n\r\n") {
        if head.len() > 4096 {
            return Err(std::io::Error::other("response head too long"));
        }
        head.push(stream.read_u8().await?);
    }
    if !head.starts_with(b"HTTP/1.1 200") {
        let status = String::from_utf8_lossy(head.split(|&b| b == b'\r').next().unwrap_or(&[]));
        return Err(std::io::Error::other(format!("refused: {status}")));
    }
    Ok(stream)
}

/// A service on a peer (`ClusterNode::peer_service`).
struct PeerService(Rc<RpcConnection>);

impl Channel for PeerService {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(attach(self.0.start_request(metadata), Rc::clone(&self.0)))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("A peer's service"))
    }
}

/// A service here that a peer's session reaches (`ServerHandle::resolve_peer_cache`): each
/// request is counted on the session, and refused once this instance stops its peers' sessions.
pub(crate) struct SessionService {
    pub(crate) node: Rc<ClusterNode>,
    pub(crate) session: Rc<Count>,
    pub(crate) service: Rc<dyn Channel>,
}

impl Channel for SessionService {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let counted = self.node.peer_request(Some(&self.session))?;
        Ok(attach(self.service.start_request(metadata)?, counted))
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("A peer's service"))
    }
}

/// Where a Durable Object runs, for a request about to start on it: here at
/// an epoch, or on the node named.
pub enum Owner {
    Here(u64),
    Remote(Rc<RpcConnection>, String),
    /// Here at the epoch, as this instance drains: it is handed off (`ClusterNode::release`),
    /// not served.
    Releasing(u64),
}

/// "Moved, not started": the answer of an instance that does not serve a Durable Object.
///
/// It is given before anything runs. It is a disconnect, which the application sees as
/// retryable, and its details tell it from one: the caller may send the request again to
/// whichever instance serves the object now.
pub fn moved(description: &str) -> crate::Error {
    kj::disconnected!("{description}").with_details(vec![
        (ffi::request_not_delivered_detail_id(), vec![]),
        (ffi::actor_predecessor_rejected_detail_id(), vec![]),
    ])
}

/// `error`, marked as a failure before the request reached the object: it may be sent again.
pub fn not_delivered(error: crate::Error) -> crate::Error {
    let mut details = error.details().cloned().unwrap_or_default();
    if details
        .iter()
        .any(|(id, _)| *id == ffi::request_not_delivered_detail_id())
    {
        return error;
    }
    details.push((ffi::request_not_delivered_detail_id(), vec![]));
    error.with_details(details)
}

/// Whether `error` is a `moved` answer.
pub fn is_moved(error: &crate::Error) -> bool {
    let has = |id| {
        error
            .details()
            .is_some_and(|d| d.iter().any(|(k, _)| *k == id))
    };
    has(ffi::request_not_delivered_detail_id()) && has(ffi::actor_predecessor_rejected_detail_id())
}

impl RpcConnection {
    /// Whether its session disconnected.
    pub fn is_down(&self) -> bool {
        self.down.get()
    }

    /// Starts a request on the remote owner; the caller keeps the session alive with it.
    pub fn start_request(
        &self,
        metadata: KjOwn<ffi::RequestMetadata>,
    ) -> KjOwn<ffi::WorkerInterface> {
        ffi::rpc_client_start_request(&self.client, metadata)
    }
}

// =====================================================================================
// The port

/// Serves the cluster port until the server is dropped.
pub async fn serve(listener: TcpListener, node: Rc<ClusterNode>) {
    let settings = Rc::new(ServerSettings::default());
    let shutdown = Rc::new(Shutdown::new());
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        let _ = stream.set_nodelay(true);
        let hangup = kj_rs_io::when_write_disconnected(&stream)
            .err_into()
            .boxed();
        let handler: Rc<dyn Handler> = Rc::new(PeerConnection {
            node: Rc::clone(&node),
        });
        let (settings, shutdown) = (Rc::clone(&settings), Rc::clone(&shutdown));
        let factory = Rc::clone(&node.factory);
        node.factory.spawn_detached(async move {
            let table = header_table(&factory);
            if let Err(e) = kj_hyper::server::serve_connection(
                stream, hangup, table, settings, handler, &shutdown,
            )
            .await
            {
                tracing::warn!("cluster connection failed: {}", e.description());
            }
        });
    }
}

struct PeerConnection {
    node: Rc<ClusterNode>,
}

impl PeerConnection {
    /// Checks a CONNECT's `AUTH_HEADER` (`check_connect_auth`).
    fn verify(&self, header: Option<&[u8]>) -> Result<()> {
        let node = &self.node;
        check_connect_auth(
            &|data| node.cluster.mac(data),
            node.cluster.node(),
            &node.advertise,
            header,
            cluster::now_ms(),
            &mut node.nonces.borrow_mut(),
        )
    }
}

#[async_trait::async_trait(?Send)]
impl Handler for PeerConnection {
    async fn request<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        _headers: HeadersRef<'a>,
        _body: Pin<&'a mut kj::io::AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let headers = Headers::new(header_table(&self.node.factory));
        if method != Method::GET || url != b"/ready" {
            return send_error(response, 404, "Not Found", &headers).await;
        }
        // Ready to take traffic: serving and not told to stop, a member of the cluster, not
        // draining.
        let cluster = &self.node.cluster;
        if self.node.ready.get() && cluster.lease_holds() && !cluster.is_draining() {
            send_error(response, 200, "OK", &headers).await
        } else {
            send_error(response, 503, "Service Unavailable", &headers).await
        }
    }

    async fn connect<'a>(
        &'a self,
        _host: &'a [u8],
        headers: HeadersRef<'a>,
        connect: Connect,
    ) -> Result<()> {
        let table = header_table(&self.node.factory);
        let empty = Headers::new(table);
        if let Err(e) = self.verify(headers.get_by_name(AUTH_HEADER)) {
            tracing::warn!("cluster CONNECT refused: {}", e.description());
            connect.reject(403, "Forbidden", HeadersRef::from(&empty), Some(0))?;
            return Ok(());
        }
        let node = &self.node;
        // A stopping instance's sessions close (`ClusterNode::stop_peer_sessions`). Refused, the
        // peer sends its requests elsewhere: none were sent.
        if node.refusing_peers.is_set() {
            let headers = HeadersRef::from(&empty);
            connect.reject(503, "Service Unavailable", headers, Some(0))?;
            return Ok(());
        }
        let _open = node.peer_sessions.add();
        let tunnel = connect.accept(200, "OK", HeadersRef::from(&empty))?;
        let requests = Rc::<Count>::default();
        let session = Box::new(PeerSession {
            requests: Rc::clone(&requests),
        });
        // Closes once the instance stops its peers' sessions and no request started on this one
        // is in flight.
        let refusing = Rc::clone(&node.refusing_peers);
        let closing = Box::new(ResultTask(
            async move {
                refusing.wait().await;
                requests.none().await;
                Err(moved("this instance stopped"))
            }
            .boxed_local(),
        ));
        // A peer holds the cluster key, so it reaches the clustered objects here.
        let factory = node.factory.raw();
        Ok(ffi::factory_accept_cluster_peer(factory, tunnel.into_kj(), session, closing).await?)
    }
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0, |acc, (x, y)| acc | (x ^ y)) == 0
}

fn text(reader: capnp::Result<capnp::text::Reader<'_>>) -> Result<String> {
    reader
        .map_err(|e| capnp(&e))?
        .to_string()
        .map_err(|e| kj::failed!("{e}"))
}

fn capnp(e: &capnp::Error) -> crate::Error {
    kj::failed!("{e}")
}

/// A session a peer has open here: the requests started on it, each counted until it ends.
pub struct PeerSession {
    pub(crate) requests: Rc<Count>,
}

/// A Rust future C++ awaits (`result_task_run`).
pub struct ResultTask(pub LocalBoxFuture<'static, Result<()>>);

pub async fn result_task_run(task: Box<ResultTask>) -> Result<()> {
    task.0.await
}

/// What makes one actor's commits durable: `sync_actor` on its object.
pub struct Committer {
    pub cluster: Rc<Member>,
    pub namespace: String,
    pub object: String,
}

impl Committer {
    pub fn commit(&self) -> Box<ResultTask> {
        let cluster = Rc::clone(&self.cluster);
        let (namespace, object) = (self.namespace.clone(), self.object.clone());
        Box::new(ResultTask(
            async move {
                cluster
                    .sync_actor(&namespace, &object)
                    .await
                    .map_err(|e| kj::failed!("{namespace}/{object}: commit failed: {e}"))
            }
            .boxed_local(),
        ))
    }
}

/// This instance's ownership of one Durable Object at an epoch.
///
/// Its container holds it while the container is in its namespace's map. Dropping it stops capturing the object and keeps it
/// dormant here, uploading what the bucket lacks; the next request wakes it.
pub struct Owned {
    node: Rc<ClusterNode>,
    namespace: String,
    actor: String,
    epoch: u64,
    /// Whether dropping it keeps the object dormant here; not once it is handed off.
    evict: Cell<bool>,
    /// The takeover, shared by the requests that wait for it.
    restore: RefCell<Option<futures::future::Shared<LocalBoxFuture<'static, Result<()>>>>>,
}

impl Owned {
    pub fn new(node: Rc<ClusterNode>, namespace: &str, actor: &str, epoch: u64) -> Self {
        Self {
            node,
            namespace: namespace.to_owned(),
            actor: actor.to_owned(),
            epoch,
            evict: Cell::new(true),
            restore: RefCell::new(None),
        }
    }

    /// Hands the object off as this instance drains, once it stopped: uploads what the bucket
    /// lacks, then releases it to the next claim. Answers whether it was released.
    pub fn hand_off(self) -> impl Future<Output = bool> + use<> {
        self.evict.set(false);
        let cluster = Rc::clone(&self.node.cluster);
        let (namespace, actor, epoch) = (self.namespace.clone(), self.actor.clone(), self.epoch);
        drop(self);
        async move {
            match cluster.hand_off_actor(&namespace, &actor, epoch).await {
                Ok(released) => released,
                Err(e) => {
                    tracing::warn!("cluster: handing off {namespace}/{actor}: {e}");
                    false
                }
            }
        }
    }

    pub fn cluster(&self) -> &Rc<Member> {
        &self.node.cluster
    }

    /// Takes the object over into the databases `layout` places, once: recovers what earlier
    /// epochs committed and opens this one.
    pub fn restore(&self, layout: Rc<dyn Layout>) -> impl Future<Output = Result<()>> + use<> {
        let mut restore = self.restore.borrow_mut();
        let cluster = Rc::clone(&self.node.cluster);
        let (namespace, actor, epoch) = (self.namespace.clone(), self.actor.clone(), self.epoch);
        restore
            .get_or_insert_with(|| {
                async move {
                    cluster
                        .restore_actor(&namespace, &actor, epoch, &*layout)
                        .await
                        .map_err(|e| kj::failed!("{namespace}/{actor}: takeover failed: {e}"))
                }
                .boxed_local()
                .shared()
            })
            .clone()
    }
}

impl Owned {
    /// What dropping this does, to run again for an object a restore opened after the drop.
    pub fn retire(&self) -> Retire {
        Retire {
            node: Rc::clone(&self.node),
            namespace: self.namespace.clone(),
            actor: self.actor.clone(),
            epoch: self.epoch,
        }
    }
}

impl Drop for Owned {
    fn drop(&mut self) {
        if self.evict.get() {
            self.retire().run();
        } else {
            self.node.cluster.close_actor(&self.namespace, &self.actor);
        }
    }
}

/// Stops capturing an object that left this node, and evicts it (`Owned::retire`).
pub struct Retire {
    node: Rc<ClusterNode>,
    namespace: String,
    actor: String,
    epoch: u64,
}

impl Retire {
    pub fn run(self) {
        let cluster = Rc::clone(&self.node.cluster);
        cluster.close_actor(&self.namespace, &self.actor);
        let (namespace, actor, epoch) = (self.namespace, self.actor, self.epoch);
        self.node.factory.spawn_detached(async move {
            cluster.evict_actor(&namespace, &actor, epoch).await;
        });
    }
}

/// One count for the lease thread to watch that stands still while any of `loops` does: it
/// advances when every loop has made progress since it last advanced.
fn every_loop(loops: Vec<cluster::Progress>) -> cluster::Progress {
    let seen = Mutex::new((vec![None::<u64>; loops.len()], 0u64));
    Box::new(move || {
        let counts: Vec<u64> = loops.iter().map(|count| count()).collect();
        let mut seen = seen.lock().unwrap_or_else(PoisonError::into_inner);
        let (last, combined) = &mut *seen;
        let all = counts
            .iter()
            .zip(last.iter())
            .all(|(count, last)| Some(*count) != *last);
        if all {
            *last = counts.into_iter().map(Some).collect();
            *combined += 1;
        }
        let combined = *combined;
        drop(seen);
        combined
    })
}

/// The control plane of an instance whose first loop thread has it, for another loop thread:
/// each call is mail to the first thread, answered by its `ClusterNode::control`.
struct RemoteControl {
    factory: Rc<Factory>,
    /// This thread's client of the instance's bucket.
    bucket: cluster::Bucket,
}

impl RemoteControl {
    /// Sends `call` to the first thread and waits for its answer.
    async fn ask(&self, call: ControlCall) -> cluster::Result<ControlAnswer> {
        let gone = || cluster::Error::Replica("the first loop thread has ended".to_owned());
        let (answer, answered) = tokio::sync::oneshot::channel();
        let mail = Mail::Control {
            from: self.factory.thread(),
            call,
            answer: Some(answer),
        };
        if !self.factory.post(0, mail) {
            return Err(gone());
        }
        answered.await.map_err(|_| gone())
    }

    /// Sends `call`, which has no answer, to the first thread: it is in effect there before
    /// anything this thread sends after it.
    fn tell(&self, call: ControlCall) {
        let mail = Mail::Control {
            from: self.factory.thread(),
            call,
            answer: None,
        };
        // A first thread that is gone has no table to change.
        let _ = self.factory.post(0, mail);
    }
}

fn unexpected<T>() -> cluster::Result<T> {
    Err(cluster::Error::Replica(
        "the control plane answered another call".to_owned(),
    ))
}

#[async_trait::async_trait(?Send)]
impl cluster::Open for RemoteControl {
    async fn open(&self, ns: &str, actor: &str, epoch: u64) -> cluster::Result<bool> {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        match self.ask(ControlCall::Open { ns, actor, epoch }).await? {
            ControlAnswer::Flag(opened) => opened,
            _ => unexpected(),
        }
    }

    async fn current_epoch(&self, ns: &str, actor: &str) -> cluster::Result<Option<u64>> {
        cluster::current_epoch(&self.bucket, ns, actor).await
    }
}

#[async_trait::async_trait(?Send)]
impl Control for RemoteControl {
    async fn claim(&self, ns: &str, actor: &str, now_ms: u64) -> cluster::Result<cluster::Claim> {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        match self.ask(ControlCall::Claim { ns, actor, now_ms }).await? {
            ControlAnswer::Claim(claim) => claim,
            _ => unexpected(),
        }
    }

    async fn advance(
        &self,
        ns: &str,
        actor: &str,
        epoch: u64,
    ) -> cluster::Result<Option<cluster::Owned>> {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        match self.ask(ControlCall::Advance { ns, actor, epoch }).await? {
            ControlAnswer::Advance(next) => next,
            _ => unexpected(),
        }
    }

    async fn release(&self, ns: &str, actor: &str, epoch: u64) -> cluster::Result<bool> {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        match self.ask(ControlCall::Release { ns, actor, epoch }).await? {
            ControlAnswer::Flag(released) => released,
            _ => unexpected(),
        }
    }

    fn abandon(&self, ns: &str, actor: &str) {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        self.tell(ControlCall::Abandon { ns, actor });
    }

    fn forget_route(&self, ns: &str, actor: &str, node: &str) {
        let (ns, actor, node) = (ns.to_owned(), actor.to_owned(), node.to_owned());
        self.tell(ControlCall::ForgetRoute { ns, actor, node });
    }

    async fn refresh_route(&self, ns: &str, actor: &str, now_ms: u64) -> cluster::Result<()> {
        let (ns, actor) = (ns.to_owned(), actor.to_owned());
        let call = ControlCall::RefreshRoute { ns, actor, now_ms };
        match self.ask(call).await? {
            ControlAnswer::Done(done) => done,
            _ => unexpected(),
        }
    }

    async fn arm_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: Option<u64>,
    ) -> cluster::Result<()> {
        let call = ControlCall::ArmAlarm {
            ns: ns.to_owned(),
            actor: actor.to_owned(),
            name: name.map(str::to_owned),
            at_ms,
        };
        match self.ask(call).await? {
            ControlAnswer::Done(done) => done,
            _ => unexpected(),
        }
    }

    async fn opened_alarm(
        &self,
        ns: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: u64,
    ) -> cluster::Result<()> {
        let call = ControlCall::OpenedAlarm {
            ns: ns.to_owned(),
            actor: actor.to_owned(),
            name: name.map(str::to_owned),
            at_ms,
        };
        match self.ask(call).await? {
            ControlAnswer::Done(done) => done,
            _ => unexpected(),
        }
    }

    async fn alarm_elsewhere(
        &self,
        ns: &str,
        actor: &str,
        owner: &str,
        now_ms: u64,
    ) -> cluster::Result<bool> {
        let call = ControlCall::AlarmElsewhere {
            ns: ns.to_owned(),
            actor: actor.to_owned(),
            owner: owner.to_owned(),
            now_ms,
        };
        match self.ask(call).await? {
            ControlAnswer::Flag(again) => again,
            _ => unexpected(),
        }
    }
}

#[cfg(test)]
#[path = "cluster-test.rs"]
mod tests;
