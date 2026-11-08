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
//! the instance takes traffic: it serves, holds its lease, and is not stopping.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::time::Duration;

use cluster::Cluster;
use cluster::Layout;
use cluster::Route;
use cluster_logic::handoff::Budget;
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
use crate::config::Factory;
use crate::services::header_table;
use crate::services::send_error;

/// A session's CONNECT carries `<ms>.<nonce>.<mac>`: see `connect_mac`.
const AUTH_HEADER: &str = "cf-cluster-auth";
/// How far a CONNECT's timestamp may be from this instance's clock.
const AUTH_WINDOW_MS: u64 = 30_000;
/// How long dialing a peer and its answer to the CONNECT may take.
const TUNNEL_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

/// This instance's cluster: the crate's state and the factory sessions to owners start from.
pub struct ClusterNode {
    pub cluster: Rc<Cluster>,
    factory: Rc<Factory>,
    /// One session per peer address, dropped when it disconnects.
    sessions: Rc<Sessions<Rc<RpcConnection>>>,
    /// The address peers dial, which their CONNECTs are bound to.
    advertise: String,
    /// The nonces of CONNECTs accepted within twice the window, by timestamp.
    nonces: RefCell<HashMap<String, u64>>,
    /// The clustered namespaces, by their part of the bucket's keys.
    namespaces: RefCell<HashMap<String, Weak<ActorNamespace>>>,
    /// How a stop's time is spent (`stopTimeoutMs`).
    budget: Budget,
    /// Set once this instance drains.
    deadlines: Cell<Option<Deadlines>>,
    /// The requests peers have in flight here.
    peer_requests: Rc<PeerRequests>,
    /// Whether the application's listeners are being served (`Serving`).
    serving: Cell<bool>,
}

/// When the phases of this instance's stop end (`cluster_logic::handoff::Budget`).
#[derive(Clone, Copy)]
pub struct Deadlines {
    pub in_flight: Instant,
    pub handoff: Instant,
    pub release: Instant,
    pub grace: Instant,
    pub connections: Instant,
    pub stop: Instant,
}

/// A count of requests, and its waiters for it to reach zero.
#[derive(Default)]
struct PeerRequests {
    count: Cell<usize>,
    none: Notify,
}

/// The application's listeners are being served, until this is dropped.
pub struct Serving(Rc<ClusterNode>);

impl Drop for Serving {
    fn drop(&mut self) {
        self.0.serving.set(false);
    }
}

/// A request a peer has in flight here, from its start until its interface is dropped.
pub struct PeerRequest(Rc<PeerRequests>);

impl Drop for PeerRequest {
    fn drop(&mut self) {
        let count = self.0.count.get() - 1;
        self.0.count.set(count);
        if count == 0 {
            self.0.none.notify_waiters();
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

/// One value per key, a peer's session: the callers that arrive while it is
/// being made share the attempt, and a failed attempt is forgotten, so the
/// next caller makes another.
pub struct Sessions<V> {
    map: RefCell<HashMap<String, Shared<Dial<V>>>>,
}

impl<V> Default for Sessions<V> {
    fn default() -> Self {
        Self {
            map: RefCell::default(),
        }
    }
}

impl<V: Clone + 'static> Sessions<V> {
    /// `key`'s value: the one made, the one being made, or one `make` makes.
    pub async fn get(
        &self,
        key: &str,
        make: impl FnOnce() -> Dial<V>,
    ) -> std::result::Result<V, String> {
        let attempt = self.map.borrow().get(key).cloned();
        let attempt = attempt.unwrap_or_else(|| {
            let attempt = make().shared();
            let mut map = self.map.borrow_mut();
            map.insert(key.to_owned(), attempt.clone());
            attempt
        });
        let result = attempt.clone().await;
        if result.is_err() {
            let mut map = self.map.borrow_mut();
            if map.get(key).is_some_and(|a| a.ptr_eq(&attempt)) {
                map.remove(key);
            }
        }
        result
    }

    /// Forgets `key`'s value if it is the one `is` matches.
    pub fn forget(&self, key: &str, is: impl Fn(&V) -> bool) {
        let mut map = self.map.borrow_mut();
        let made = map.get(key).and_then(|a| a.peek());
        if made.is_some_and(|r| r.as_ref().is_ok_and(&is)) {
            map.remove(key);
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

/// The MAC a CONNECT to `target` carries, at `ms` with `nonce`: a header seen on the wire is
/// refused at any other target, once used, or once stale.
fn connect_mac(cluster: &Cluster, target: &str, ms: u64, nonce: &str) -> Vec<u8> {
    cluster.mac(format!("workerd cluster connect v1\n{target}\n{ms}\n{nonce}").as_bytes())
}

impl ClusterNode {
    /// Joins the cluster: the crate's state for `conf`, this instance at `address`.
    pub fn new(
        factory: Rc<Factory>,
        conf: cluster_config::Reader<'_>,
        address: String,
    ) -> Result<Rc<Self>> {
        let options = conf.get_bucket_options().map_err(|e| capnp(&e))?;
        let setup = cluster::Setup {
            url: text(conf.get_bucket())?,
            options: options.iter().map(text).collect::<Result<_>>()?,
            node: cluster::hex(&rand::random::<[u8; 16]>()),
            address: address.clone(),
            ttl_ms: cluster_logic::lease::LEASE_MS,
            key: text(conf.get_key())?.into_bytes(),
        };
        let cluster = Cluster::new(setup).map_err(|e| kj::failed!("cluster: {e}"))?;
        Ok(Rc::new(Self {
            cluster,
            factory,
            sessions: Rc::default(),
            advertise: address,
            nonces: RefCell::default(),
            namespaces: RefCell::default(),
            budget: Budget {
                stop_ms: conf.get_stop_timeout_ms().into(),
            },
            deadlines: Cell::new(None),
            peer_requests: Rc::default(),
            serving: Cell::new(false),
        }))
    }

    /// Marks the application's listeners as served until the guard is dropped.
    pub fn serving(self: &Rc<Self>) -> Serving {
        self.serving.set(true);
        Serving(Rc::clone(self))
    }

    /// Starts draining: this instance claims nothing more, and its stop's deadlines run from now.
    pub fn start_draining(&self) -> Deadlines {
        let now = Instant::now();
        let at = |ms| now + Duration::from_millis(ms);
        let deadlines = Deadlines {
            in_flight: at(self.budget.in_flight_ms()),
            handoff: at(self.budget.handoff_ms()),
            release: at(self.budget.release_ms()),
            grace: at(self.budget.grace_ms()),
            connections: at(self.budget.connections_ms()),
            stop: at(self.budget.stop_ms),
        };
        self.deadlines.set(Some(deadlines));
        self.cluster.start_draining();
        let cluster = Rc::clone(&self.cluster);
        self.factory
            .spawn_detached(async move { cluster.announce_draining().await });
        deadlines
    }

    /// Hands off an object this draining instance owns at `epoch` but does not run, once it is
    /// at rest here, unless the handoff's deadline passes first. Answers whether it was released;
    /// one that was not is abandoned (`Cluster::abandon_actor`).
    pub async fn release(&self, namespace: &str, actor: &str, epoch: u64) -> bool {
        let Some(deadlines) = self.deadlines.get() else {
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

    /// Counts a request a peer makes here until the guard is dropped.
    pub fn peer_request(&self) -> PeerRequest {
        let requests = Rc::clone(&self.peer_requests);
        requests.count.set(requests.count.get() + 1);
        PeerRequest(requests)
    }

    /// Resolves once no peer has a request in flight here.
    pub async fn peer_requests_done(&self) {
        loop {
            let none = self.peer_requests.none.notified();
            if self.peer_requests.count.get() == 0 {
                return;
            }
            none.await;
        }
    }

    pub fn add_namespace(&self, unique_key: &str, namespace: Weak<ActorNamespace>) {
        let hash = cluster::namespace_hash(unique_key);
        self.namespaces.borrow_mut().insert(hash, namespace);
    }

    /// Whether this instance is handing off `object`, named `<namespace hash>/<root key>`.
    pub fn hands_off(&self, object: &str) -> bool {
        let Some((hash, key)) = object.split_once('/') else {
            return false;
        };
        let namespace = self.namespaces.borrow().get(hash).and_then(Weak::upgrade);
        namespace.is_some_and(|namespace| namespace.hands_off(key))
    }

    /// Keeps this instance's alarm index tidy and schedules its share of dead peers' alarms.
    /// Never returns.
    pub async fn alarms_loop(&self) {
        loop {
            tokio::time::sleep(ALARMS_INTERVAL).await;
            // A draining instance takes on no alarms; its own go to its peers as it leaves.
            if self.cluster.is_draining() {
                continue;
            }
            let schedule = |alarm: cluster::Adopted| {
                let namespace = self.namespaces.borrow().get(&alarm.namespace).cloned();
                let Some(namespace) = namespace.and_then(|ns| ns.upgrade()) else {
                    // Not configured here: a peer that has it adopts it once this one dies.
                    tracing::warn!("an adopted alarm's namespace is not configured here");
                    return;
                };
                let (actor, name) = (&alarm.actor, alarm.name.as_deref());
                if let Err(e) = namespace.adopt_alarm(actor, name, alarm.at_ms) {
                    tracing::warn!("scheduling an adopted alarm: {}", e.description());
                }
            };
            self.cluster.alarms_round(cluster::now_ms(), schedule).await;
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
        let session = self.session(&address).await.map_err(|e| {
            // Unreachable: its own record, not the listing, tells whether it still owns the
            // object (`forget_route`).
            self.cluster.forget_route(namespace, actor, &node);
            kj::disconnected!("{namespace}/{actor} on {node}: {e}")
        })?;
        // A channel per stub, so a stub stays with the instance it reached.
        let connection = RpcConnection {
            client: ffi::rpc_client_debug_actor(&session.client, service, class, actor),
            down: Rc::clone(&session.down),
        };
        Ok(Owner::Remote(Rc::new(connection), node))
    }

    /// The session to the peer at `address`: the open one, the one being
    /// dialed, or a new one.
    async fn session(&self, address: &str) -> std::io::Result<Rc<RpcConnection>> {
        let dial = || self.dial(address);
        let session = self.sessions.get(address, dial).await;
        session.map_err(std::io::Error::other)
    }

    fn dial(&self, address: &str) -> Dial<Rc<RpcConnection>> {
        let cluster = Rc::clone(&self.cluster);
        let factory = Rc::clone(&self.factory);
        let sessions = Rc::clone(&self.sessions);
        let address = address.to_owned();
        async move {
            // A stalled peer accepts the connection and never answers.
            let tunnel = tokio::time::timeout(TUNNEL_TIMEOUT, tunnel(&cluster, &address));
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
                sessions.forget(&address, |s| Rc::ptr_eq(s, &watched));
            });
            Ok(session)
        }
        .boxed_local()
    }
}

async fn tunnel(node: &Cluster, address: &str) -> std::io::Result<TcpStream> {
    let mut stream = TcpStream::connect(address).await?;
    stream.set_nodelay(true)?;
    let ms = cluster::now_ms();
    let nonce = cluster::hex(&rand::random::<[u8; 16]>());
    let mac = cluster::hex(&connect_mac(node, address, ms, &nonce));
    let auth = format!("{AUTH_HEADER}: {ms}.{nonce}.{mac}");
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
    /// Checks a CONNECT's `AUTH_HEADER`: its MAC, its clock, and that its nonce is new.
    fn verify(&self, header: Option<&[u8]>) -> Result<()> {
        let bad = || kj::failed!("cluster CONNECT without a valid {AUTH_HEADER}");
        let header = header
            .and_then(|h| std::str::from_utf8(h).ok())
            .ok_or_else(bad)?;
        let [ms, nonce, mac] = header.split('.').collect::<Vec<_>>()[..] else {
            return Err(bad());
        };
        let ms: u64 = ms.parse().map_err(|_| bad())?;
        let mac = cluster::unhex(mac).ok_or_else(bad)?;
        let node = &self.node;
        if !constant_time_eq(
            &connect_mac(&node.cluster, &node.advertise, ms, nonce),
            &mac,
        ) {
            return Err(bad());
        }
        let now = cluster::now_ms();
        if now.abs_diff(ms) > AUTH_WINDOW_MS {
            return Err(kj::failed!(
                "cluster CONNECT {}ms off this clock",
                now.abs_diff(ms)
            ));
        }
        let mut nonces = node.nonces.borrow_mut();
        nonces.retain(|_, t| now.abs_diff(*t) <= 2 * AUTH_WINDOW_MS);
        if nonces.insert(nonce.to_owned(), ms).is_some() {
            return Err(kj::failed!("cluster CONNECT replayed"));
        }
        Ok(())
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
        // Ready to take traffic: serving, a member of the cluster, not stopping.
        let cluster = &self.node.cluster;
        if self.node.serving.get() && cluster.lease_holds() && !cluster.is_draining() {
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
        let tunnel = connect.accept(200, "OK", HeadersRef::from(&empty))?;
        // A peer holds the cluster key, so it gets the debug port's interface.
        Ok(ffi::factory_accept_debug_port(self.node.factory.raw(), tunnel.into_kj()).await?)
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

/// A Rust future C++ awaits (`result_task_run`).
pub struct ResultTask(pub LocalBoxFuture<'static, Result<()>>);

pub async fn result_task_run(task: Box<ResultTask>) -> Result<()> {
    task.0.await
}

/// What makes one actor's commits durable: `sync_actor` on its object.
pub struct Committer {
    pub cluster: Rc<Cluster>,
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

    pub fn cluster(&self) -> &Rc<Cluster> {
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

#[cfg(test)]
#[path = "cluster-test.rs"]
mod tests;
