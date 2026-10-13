// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The external and network services: leaf channels of the service graph whose requests leave
//! the process over HTTP or TCP.
//!
//! Each is an `Rc` of shared state; `start_request` wraps it in a fresh `worker::Interface` for
//! one event. Outbound HTTP goes through kj-hyper's pooled client, which borrows the factory's
//! header table, so a client is built per event over the service's dialer or peer filter; the
//! TLS configuration, the expensive part, is built once per service.

pub mod disk;
pub mod network;
pub mod rewriter;

use std::cell::RefCell;
use std::pin::Pin;
use std::rc::Rc;
use std::rc::Weak;
use std::sync::Arc;
use std::time::SystemTime;

use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_hyper::WebSocketCompression;
use kj_hyper::client::Client;
use kj_hyper::client::ClientSettings;
use kj_hyper::client::Dialed;
use kj_hyper::client::Peer;
use kj_hyper::client::connect_allowed;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use kj_rs_io::Socket;
use kj_rs_io::TokioAddress;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::external_server;

use crate::Error;
use crate::Result;
use crate::bridge;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::Factory;
use crate::config::capnp_error;
use crate::config::optional_text;
use crate::config::text;
use crate::listen::host_of_address;
use crate::listen::loopback::Loopback;
pub use crate::services::disk::DiskDirectoryService;
pub use crate::services::disk::make_disk_directory_service;
use crate::services::network::PeerFilter;
use crate::services::network::tls_options;
use crate::services::rewriter::HttpRewriter;
use crate::services::rewriter::Style;

// =======================================================================================
// Shared pieces

/// The header table every `kj::HttpHeaders` a worker sees is built against.
pub fn header_table(factory: &Factory) -> &HeaderTable {
    ffi::factory_header_table(factory.raw())
}

/// A leaf service cannot be handed to another worker as a stub (`DataCloneError`).
pub fn not_transferable(what: &str) -> Error {
    kj::failed!("jsg.DOMException(DataCloneError): {what} can't be passed over RPC.")
}

/// The error a leaf service answers events it has no handler for with.
pub fn unsupported(what: &str) -> Error {
    kj::failed!("jsg.Error: {what} don't support this event type.")
}

/// `kj::HttpService::Response::sendError`: the status text as the body.
pub async fn send_error<'h>(
    response: ServiceResponse<'_>,
    status: u32,
    text: &str,
    headers: impl Into<HeadersRef<'h>>,
) -> Result<()> {
    let mut body = response.send(status, text, headers, Some(text.len() as u64))?;
    body.write(text.as_bytes()).await
}

/// The request's cf blob, as JSON, when the runtime attached one.
fn cf_blob_json(metadata: &RequestMetadata) -> Option<String> {
    ffi::request_metadata_cf_blob_json(metadata).into()
}

/// kj's client settings for these services: the application negotiates WebSocket compression
/// itself.
fn client_settings() -> ClientSettings {
    ClientSettings {
        websocket_compression: WebSocketCompression::MANUAL,
        websocket_errors: Some(ffi::new_jsgify_websocket_errors()),
        ..ClientSettings::default()
    }
}

// =======================================================================================
// Dialing one peer

/// TLS to one peer: the configuration and the name the certificate must carry.
struct Tls {
    config: Arc<rustls::ClientConfig>,
    server_name: String,
}

/// Connects to one configured peer: its address in kj-rs-io's grammar, resolved each time, or
/// a `loopback:` name, a socket of this server (`listen::loopback`).
struct Dialer {
    address: String,
    default_port: u16,
    loopback: Loopback,
    tls: Option<Tls>,
}

/// A loopback connection nobody listens for, as the refusal a socket would report.
#[expect(clippy::needless_pass_by_value, reason = "a `map_err` adapter")]
fn refused(error: Error) -> std::io::Error {
    std::io::Error::new(
        std::io::ErrorKind::ConnectionRefused,
        error.description().to_owned(),
    )
}

impl Dialer {
    /// `certificate_host` is the name TLS verifies, else the address's host.
    fn new(
        address: &str,
        default_port: u16,
        tls: Option<(Arc<rustls::ClientConfig>, Option<String>)>,
        loopback: &Loopback,
    ) -> Self {
        let tls = tls.map(|(config, certificate_host)| Tls {
            config,
            server_name: certificate_host
                .unwrap_or_else(|| host_of_address(address).trim_matches(['[', ']']).to_owned()),
        });
        Self {
            address: address.to_owned(),
            default_port,
            loopback: loopback.clone(),
            tls,
        }
    }

    /// The connection, with its hang-up signal taken before any TLS.
    async fn dial(&self) -> std::io::Result<Dialed> {
        let stream: Dialed = if let Some(name) = self.address.strip_prefix("loopback:") {
            self.loopback.connect(name).map_err(refused)?.into()
        } else {
            let address = TokioAddress::parse_str(&self.address, self.default_port).await?;
            match address.connect_first().await? {
                Socket::Tcp(stream) => stream.into(),
                #[cfg(unix)]
                Socket::Unix(stream) => stream.into(),
            }
        };
        match &self.tls {
            Some(tls) => stream.tls(Arc::clone(&tls.config), &tls.server_name).await,
            None => Ok(stream),
        }
    }
}

/// `settings` reborrowed for a shorter lifetime, for a client that lives only for the call: the
/// bridge struct is invariant in its lifetime, so it cannot shrink on its own.
fn reborrow_settings<'s>(
    use_tls: bool,
    tls_starter: &'s mut Option<Pin<&mut kj::http::ffi::TlsStarterCallback>>,
) -> ConnectSettings<'s> {
    ConnectSettings {
        use_tls,
        tls_starter: tls_starter.as_mut().map(Pin::as_mut).into(),
    }
}

/// A kj-hyper client of the dialer's peer, asked for URLs in `style`.
fn fixed_client<'t>(table: &'t HeaderTable, dialer: &Arc<Dialer>, style: Style) -> Client<'t> {
    let dialer = Arc::clone(dialer);
    let dial = move || {
        let dialer = Arc::clone(&dialer);
        async move { dialer.dial().await }
    };
    let peer = match style {
        Style::Host => Peer::Origin,
        Style::Proxy => Peer::Proxy,
    };
    Client::new(table, client_settings(), peer, dial)
}

// =======================================================================================
// External HTTP

/// A capnp connection to the external server, over an HTTP CONNECT tunnel.
struct RpcConnection(KjOwn<ffi::RpcClient>);

struct ExternalHttp {
    factory: Rc<Factory>,
    dialer: Arc<Dialer>,
    rewriter: HttpRewriter,
    /// The capnp connection custom events go over, made when the first one needs it and dropped
    /// when it is lost.
    rpc: RefCell<Option<Rc<RpcConnection>>>,
}

impl ExternalHttp {
    /// The capnp connection, opened if there is none. A connection that is lost is forgotten,
    /// so the next event opens a new one.
    async fn rpc(self: &Rc<Self>) -> Result<Rc<RpcConnection>> {
        if let Some(connection) = &*self.rpc.borrow() {
            return Ok(Rc::clone(connection));
        }
        let host = self
            .rewriter
            .capnp_connect_host()
            .ok_or_else(|| kj::failed!("jsg.Error: This ExternalServer not configured for RPC."))?;
        // The tunnel is an HTTP CONNECT to the external server.
        let table = header_table(&self.factory);
        let client = fixed_client(table, &self.dialer, self.rewriter.style());
        let tunnel = client.tunnel(host).await?;
        let connection = Rc::new(RpcConnection(ffi::new_rpc_client(
            self.factory.raw(),
            tunnel,
        )));
        *self.rpc.borrow_mut() = Some(Rc::clone(&connection));
        let service: Weak<Self> = Rc::downgrade(self);
        let watched = Rc::clone(&connection);
        self.factory.spawn_detached(async move {
            // A failure of the wait itself is a lost connection too.
            let _ = ffi::rpc_client_on_disconnect(&watched.0).await;
            if let Some(service) = service.upgrade() {
                let mut current = service.rpc.borrow_mut();
                if current.as_ref().is_some_and(|c| Rc::ptr_eq(c, &watched)) {
                    *current = None;
                }
            }
        });
        Ok(connection)
    }
}

/// An `external` service speaking HTTP or HTTPS.
pub struct ExternalHttpService(Rc<ExternalHttp>);

impl Channel for ExternalHttpService {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(ExternalHttpRequest {
            service: Rc::clone(&self.0),
            metadata,
        }
        .into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("ExternalService"))
    }
}

/// One event on an [`ExternalHttpService`].
struct ExternalHttpRequest {
    service: Rc<ExternalHttp>,
    metadata: KjOwn<RequestMetadata>,
}

#[async_trait::async_trait(?Send)]
impl Service for ExternalHttpRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let table = header_table(&self.service.factory);
        let rewriter = &self.service.rewriter;
        let url = std::str::from_utf8(url).map_err(|_| kj::failed!("request URL is not UTF-8"))?;
        let rewritten = rewriter.rewrite_outgoing_request(
            table,
            url,
            headers,
            cf_blob_json(&self.metadata).as_deref(),
        )?;
        let headers = rewritten
            .headers
            .as_deref()
            .map_or(headers, HeadersRef::from);
        let mut client = fixed_client(table, &self.service.dialer, rewriter.style());
        let mut response =
            bridge::rewriting_response(response.into_ffi(), table, rewriter.response_edits())?;
        client
            .request(
                method,
                rewritten.url.as_bytes(),
                headers,
                request_body,
                ServiceResponse::from(response.as_mut()),
            )
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
        let table = header_table(&self.service.factory);
        let style = self.service.rewriter.style();
        let mut client = fixed_client(table, &self.service.dialer, style);
        let mut tls_starter = settings.tls_starter.into();
        let settings = reborrow_settings(settings.use_tls, &mut tls_starter);
        client
            .connect(host, headers, connection, response, settings)
            .await
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for ExternalHttpRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("External HTTP servers"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("External HTTP servers"))
    }

    /// Custom events go over capnp RPC to the peer's `WorkerdBootstrap`.
    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        let connection = self.service.rpc().await?;
        let result = ffi::rpc_client_custom_event(
            &connection.0,
            event,
            cf_blob_json(&self.metadata).as_deref().into(),
        )
        .await?;
        Ok(result.into())
    }
}

// =======================================================================================
// External TCP

struct ExternalTcp {
    factory: Rc<Factory>,
    dialer: Arc<Dialer>,
}

/// An `external` service speaking raw TCP: `connect` is its one event.
pub struct ExternalTcpService(Rc<ExternalTcp>);

impl Channel for ExternalTcpService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(ExternalTcpRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("ExternalService"))
    }
}

struct ExternalTcpRequest(Rc<ExternalTcp>);

#[async_trait::async_trait(?Send)]
impl Service for ExternalTcpRequest {
    async fn request<'a>(
        &'a mut self,
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _request_body: Pin<&'a mut AsyncInputStream>,
        _response: ServiceResponse<'a>,
    ) -> Result<()> {
        Err(unsupported("External TCP servers"))
    }

    /// The tunnel is the connection to the server itself, pumped both ways until either side
    /// ends.
    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        let stream = self.0.dialer.dial().await.map_err(|e| io_kj_error(&e))?;
        response.accept(200, "OK", &Headers::new(header_table(&self.0.factory)))?;
        Ok(kj_hyper::ffi::pump_tunnel(connection, stream.into_kj()).await?)
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for ExternalTcpRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("External TCP servers"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("External TCP servers"))
    }
}

// =======================================================================================
// Network

struct Network {
    factory: Rc<Factory>,
    filter: Arc<PeerFilter>,
    /// None when the config has no `tlsOptions`: `https` URLs then fail.
    tls: Option<Arc<rustls::ClientConfig>>,
}

/// A `network` service: connections to whatever host a request names, within the filter.
pub struct NetworkService(Rc<Network>);

impl Channel for NetworkService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(NetworkRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("NetworkService"))
    }
}

struct NetworkRequest(Rc<Network>);

impl NetworkRequest {
    fn client(&self) -> Client<'_> {
        let filter = Arc::clone(&self.0.filter);
        let loopback = self.0.factory.loopback().clone();
        let connect = move |host: String, port| {
            let filter = Arc::clone(&filter);
            let loopback = loopback.clone();
            async move {
                if loopback.mocks_internet() {
                    let name = if port == 80 {
                        host
                    } else {
                        format!("{host}:{port}")
                    };
                    return Ok(loopback.connect(&name).map_err(refused)?.into());
                }
                let stream = connect_allowed(&host, port, |peer| filter.allows(peer.ip())).await?;
                Ok(Dialed::from(stream))
            }
        };
        Client::internet(
            header_table(&self.0.factory),
            client_settings(),
            self.0.tls.clone(),
            connect,
        )
    }
}

#[async_trait::async_trait(?Send)]
impl Service for NetworkRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let mut client = self.client();
        client
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
        let mut client = self.client();
        let mut tls_starter = settings.tls_starter.into();
        let settings = reborrow_settings(settings.use_tls, &mut tls_starter);
        client
            .connect(host, headers, connection, response, settings)
            .await
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for NetworkRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("External HTTP servers"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("External HTTP servers"))
    }
}

// =======================================================================================
// Construction

fn tls_config(conf: workerd_capnp::tls_options::Reader<'_>) -> Result<Arc<rustls::ClientConfig>> {
    kj_hyper::tls::client_config(&tls_options(conf)?)
}

/// An `external` service: an HTTP or TCP server elsewhere. `address_override` is the CLI's
/// `--external-addr` for this service.
pub fn make_external_service(
    name: &str,
    conf: external_server::Reader<'_>,
    address_override: Option<&str>,
    factory: &Rc<Factory>,
) -> Result<Rc<dyn Channel>> {
    let address = match address_override {
        Some(address) => address.to_owned(),
        None if conf.has_address() => text(conf.get_address())?,
        None => {
            return Err(kj::failed!(
                "External service \"{name}\" has no address in the config, so must be specified \
                 on the command line with `--external-addr`."
            ));
        }
    };
    let http = |rewriter: HttpRewriter, dialer: Dialer| -> Rc<dyn Channel> {
        Rc::new(ExternalHttpService(Rc::new(ExternalHttp {
            factory: Rc::clone(factory),
            dialer: Arc::new(dialer),
            rewriter,
            rpc: RefCell::new(None),
        })))
    };
    match conf.which() {
        Ok(external_server::Which::Http(options)) => {
            let rewriter = HttpRewriter::new(options.map_err(capnp_error)?)?;
            Ok(http(
                rewriter,
                Dialer::new(&address, 80, None, factory.loopback()),
            ))
        }
        Ok(external_server::Which::Https(https)) => {
            let rewriter = HttpRewriter::new(https.get_options().map_err(capnp_error)?)?;
            let tls = tls_config(https.get_tls_options().map_err(capnp_error)?)?;
            let host = optional_text(https.has_certificate_host(), https.get_certificate_host())?;
            Ok(http(
                rewriter,
                Dialer::new(&address, 443, Some((tls, host)), factory.loopback()),
            ))
        }
        Ok(external_server::Which::Tcp(tcp)) => {
            let tls = if tcp.has_tls_options() {
                let host = optional_text(tcp.has_certificate_host(), tcp.get_certificate_host())?;
                Some((
                    tls_config(tcp.get_tls_options().map_err(capnp_error)?)?,
                    host,
                ))
            } else {
                None
            };
            Ok(Rc::new(ExternalTcpService(Rc::new(ExternalTcp {
                factory: Rc::clone(factory),
                dialer: Arc::new(Dialer::new(&address, 80, tls, factory.loopback())),
            }))))
        }
        Err(capnp::NotInSchema(_)) => Err(kj::failed!(
            "External service named \"{name}\" has unrecognized protocol. Was the config compiled \
             with a newer version of the schema?"
        )),
    }
}

fn network_service(
    factory: &Rc<Factory>,
    filter: PeerFilter,
    tls: Option<Arc<rustls::ClientConfig>>,
) -> Rc<dyn Channel> {
    Rc::new(NetworkService(Rc::new(Network {
        factory: Rc::clone(factory),
        filter: Arc::new(filter),
        tls,
    })))
}

fn texts(list: capnp::Result<capnp::text_list::Reader<'_>>) -> Result<Vec<String>> {
    list.map_err(capnp_error)?.iter().map(text).collect()
}

/// A `network` service: outbound connections to the addresses `conf` allows.
pub fn make_network_service(
    conf: workerd_capnp::network::Reader<'_>,
    factory: &Rc<Factory>,
) -> Result<Rc<dyn Channel>> {
    let allow = texts(conf.get_allow())?;
    let deny = texts(conf.get_deny())?;
    let filter = PeerFilter::new(
        allow.iter().map(String::as_str),
        deny.iter().map(String::as_str),
    )?;
    let tls = if conf.has_tls_options() {
        Some(tls_config(conf.get_tls_options().map_err(capnp_error)?)?)
    } else {
        None
    };
    Ok(network_service(factory, filter, tls))
}

/// The service the config gets when it defines none named "internet": the public network
/// (`allow = ["public"]`) with TLS through the system trust store.
pub fn make_default_network_service(factory: &Rc<Factory>) -> Result<Rc<dyn Channel>> {
    let filter = PeerFilter::new(["public"], [])?;
    let tls = kj_hyper::tls::client_config(&kj_hyper::tls::TlsOptions {
        trust_browser_cas: true,
        ..kj_hyper::tls::TlsOptions::default()
    })?;
    Ok(network_service(factory, filter, Some(tls)))
}
