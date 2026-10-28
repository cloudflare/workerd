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
//! does not own routes on through its own channel to the owner.

use std::cell::RefCell;
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;

use cluster::Cluster;
use cluster::Route;
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
use workerd_capnp::cluster_config;

use crate::Result;
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
    /// One session per peer address, dialing or open, dropped when it disconnects.
    sessions: Rc<RefCell<HashMap<String, Session>>>,
    /// The address peers dial, which their CONNECTs are bound to.
    advertise: String,
    /// The nonces of CONNECTs accepted within twice the window, by timestamp.
    nonces: RefCell<HashMap<String, u64>>,
}

pub struct RpcConnection(KjOwn<ffi::RpcClient>);

/// A session to a peer, shared by the requests that wait for its dial.
type Session = Shared<LocalBoxFuture<'static, std::result::Result<Rc<RpcConnection>, String>>>;

/// Binds the cluster port, before the services start.
/// Returns the listener and the address to advertise.
pub async fn bind(
    conf: cluster_config::Reader<'_>,
    address: Option<&str>,
    advertise: Option<&str>,
) -> Result<(TcpListener, String)> {
    let address = address.map_or_else(|| text(conf.get_address()), |a| Ok(a.to_owned()))?;
    refuse_public(conf, &address)?;
    let listener = TcpListener::bind(&address)
        .await
        .map_err(|e| kj::failed!("cluster address {address}: {e}"))?;
    let bound = listener.local_addr().map_err(|e| kj::failed!("{e}"))?;
    let advertise = match advertise {
        Some(a) => a.to_owned(),
        None if conf.has_advertise() => text(conf.get_advertise())?,
        None if bound.ip().is_unspecified() => {
            return Err(kj::failed!(
                "cluster address {address} is a wildcard, which peers cannot reach: set \
                 cluster.advertise or --cluster-advertise"
            ));
        }
        None => bound.to_string(),
    };
    refuse_public(conf, &advertise)?;
    Ok((listener, advertise))
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
        }))
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
            .map_err(|e| kj::disconnected!("{namespace}/{actor}: {e}"))?;
        let (node, address, stale) = match route {
            Route::Owned(epoch) => return Ok(Owner::Here(epoch)),
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
        let session = self
            .session(&address)
            .await
            .map_err(|e| kj::disconnected!("{namespace}/{actor} on {node}: {e}"))?;
        // A channel per stub, so a stub stays with the instance it reached.
        Ok(Owner::Remote(Rc::new(RpcConnection(
            ffi::rpc_client_debug_actor(&session.0, service, class, actor),
        ))))
    }

    /// The session to the peer at `address`: the open one, the one being
    /// dialed, or a new one.
    async fn session(&self, address: &str) -> std::io::Result<Rc<RpcConnection>> {
        let session = self.sessions.borrow().get(address).cloned();
        let session = session.unwrap_or_else(|| {
            let session = self.dial(address);
            let mut sessions = self.sessions.borrow_mut();
            sessions.insert(address.to_owned(), session.clone());
            session
        });
        let result = session.clone().await;
        if result.is_err() {
            // The next request dials again.
            let mut sessions = self.sessions.borrow_mut();
            if sessions.get(address).is_some_and(|s| s.ptr_eq(&session)) {
                sessions.remove(address);
            }
        }
        result.map_err(std::io::Error::other)
    }

    fn dial(&self, address: &str) -> Session {
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
            let session = Rc::new(RpcConnection(ffi::new_rpc_client(
                factory.raw(),
                Dialed::from(stream).into_kj(),
            )));
            let watched = Rc::clone(&session);
            factory.spawn_detached(async move {
                let _ = ffi::rpc_client_on_disconnect(&watched.0).await;
                let mut sessions = sessions.borrow_mut();
                let open = |s: &Session| matches!(s.peek(), Some(Ok(c)) if Rc::ptr_eq(c, &watched));
                if sessions.get(&address).is_some_and(open) {
                    sessions.remove(&address);
                }
            });
            Ok(session)
        }
        .boxed_local()
        .shared()
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

/// Where a Durable Object runs, for a request about to start on it.
pub enum Owner {
    Here(u64),
    Remote(Rc<RpcConnection>),
}

impl RpcConnection {
    /// Starts a request on the remote owner; the caller keeps the session alive with it.
    pub fn start_request(
        &self,
        metadata: KjOwn<ffi::RequestMetadata>,
    ) -> KjOwn<ffi::WorkerInterface> {
        ffi::rpc_client_start_request(&self.0, metadata)
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
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _body: Pin<&'a mut kj::io::AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let table = header_table(&self.node.factory);
        send_error(response, 404, "Not Found", &Headers::new(table)).await
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
            restore: RefCell::new(None),
        }
    }

    pub fn cluster(&self) -> &Rc<Cluster> {
        &self.node.cluster
    }

    /// Takes the object over into the database at `path`, once: recovers what earlier epochs
    /// committed and opens this one.
    pub fn restore(&self, path: String) -> impl Future<Output = Result<()>> + use<> {
        let mut restore = self.restore.borrow_mut();
        let cluster = Rc::clone(&self.node.cluster);
        let (namespace, actor, epoch) = (self.namespace.clone(), self.actor.clone(), self.epoch);
        restore
            .get_or_insert_with(|| {
                async move {
                    cluster
                        .restore_actor(&namespace, &actor, epoch, &path)
                        .await
                        .map_err(|e| kj::failed!("{namespace}/{actor}: takeover failed: {e}"))
                }
                .boxed_local()
                .shared()
            })
            .clone()
    }
}

impl Drop for Owned {
    fn drop(&mut self) {
        let cluster = Rc::clone(&self.node.cluster);
        cluster.close_actor(&self.namespace, &self.actor);
        let (namespace, actor, epoch) = (self.namespace.clone(), self.actor.clone(), self.epoch);
        self.node.factory.spawn_detached(async move {
            cluster.evict_actor(&namespace, &actor, epoch).await;
        });
    }
}
