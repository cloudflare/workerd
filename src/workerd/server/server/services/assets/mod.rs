// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `assets` service: a directory of static assets, served as Cloudflare serves a Worker's
//! `assets`.
//!
//! `assets_router` decides everything; this module does the I/O: it stats the files a request looks
//! at, streams the one it serves, and forwards to the user Worker. Nothing is cached, so a
//! change to the directory is seen at once.

use std::cell::OnceCell;
use std::fs::Metadata;
use std::hash::DefaultHasher;
use std::hash::Hash;
use std::hash::Hasher;
use std::path::PathBuf;
use std::pin::Pin;
use std::rc::Rc;
use std::time::SystemTime;

use assets_router::Config;
use assets_router::Deployment;
use assets_router::Request;
use assets_router::Response;
use assets_router::ResponseHeader;
use assets_router::decide::Route;
use assets_router::decide::RuleRoute;
use assets_router::mitigations::is_cdn_cgi_backslash_bypass;
use assets_router::serve::Conditions;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::http::ServiceResult;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj::io::AsyncOutputStream;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::CxxWorkerInterface;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::assets_service;

use crate::Result;
use crate::bindings::Designator;
use crate::bridge::image_filter_response;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::Factory;
use crate::config::Reporter;
use crate::config::capnp_error;
use crate::config::text;
use crate::services::disk::copy_file;
use crate::services::disk::header_value;
use crate::services::disk::is_entry_name;
use crate::services::disk::kj_headers;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::unsupported;

/// An `assets` service.
pub struct Assets {
    factory: Rc<Factory>,
    root: PathBuf,
    config: Config,
    user_worker: Option<Designator>,
    user: OnceCell<Rc<dyn Channel>>,
}

impl Assets {
    /// The file at `path`, relative to the directory, if it is an asset.
    fn stat(&self, path: &str) -> Option<(PathBuf, Metadata)> {
        let relative = path.strip_prefix('/')?;
        if !relative.split('/').all(is_entry_name) || self.config.ignored.ignored(path) {
            return None;
        }
        let path = self.root.join(relative);
        let metadata = std::fs::metadata(&path).ok().filter(Metadata::is_file)?;
        Some((path, metadata))
    }

    /// Sends a response's head, with a `Content-Length` of `length`.
    fn send<'a>(
        &self,
        response: ServiceResponse<'a>,
        status: u16,
        headers: &[ResponseHeader],
        length: u64,
    ) -> Result<AsyncOutputStream<'a>> {
        let mut map = http::HeaderMap::new();
        for ResponseHeader { name, value } in headers {
            let name = http::HeaderName::from_bytes(name.as_bytes())
                .map_err(|_| kj::failed!("invalid header name: {name}"))?;
            map.append(name, header_value(value)?);
        }
        map.insert(http::header::CONTENT_LENGTH, length.into());
        let headers = kj_headers(header_table(&self.factory), &map)?;
        let text = http::StatusCode::from_u16(status)
            .ok()
            .and_then(|s| s.canonical_reason());
        let headers = HeadersRef::from(&*headers);
        response.send(
            status.into(),
            text.unwrap_or_default(),
            headers,
            Some(length),
        )
    }
}

/// A hash of `path`, the size, and the modification time to the nanosecond. With the path in
/// it, no two files share an `ETag`, which the router takes as the same asset.
fn etag(path: &str, metadata: &Metadata) -> String {
    let nanos = metadata
        .modified()
        .ok()
        .and_then(|modified| modified.duration_since(SystemTime::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_nanos());
    let mut hasher = DefaultHasher::new();
    path.hash(&mut hasher);
    format!("{:x}-{:x}-{nanos:x}", hasher.finish(), metadata.len())
}

impl Assets {
    /// The router, or with `serve_only` the channel that serves files only.
    pub fn channel(self: &Rc<Self>, serve_only: bool) -> Rc<dyn Channel> {
        let assets = Rc::clone(self);
        Rc::new(AssetsChannel { assets, serve_only })
    }

    /// Resolves the user Worker, once every service exists.
    pub fn link(&self, lookup: impl FnOnce(&Designator) -> Rc<dyn Channel>) {
        if let Some(designator) = &self.user_worker {
            self.user.get_or_init(|| lookup(designator));
        }
    }
}

pub fn make_assets_service(
    name: &str,
    index: u32,
    conf: assets_service::Reader<'_>,
    factory: &Rc<Factory>,
    report: &Reporter,
) -> Result<Rc<Assets>> {
    let path = text(conf.get_path())?;
    let root = std::env::current_dir()
        .map_err(|e| io_kj_error(&e))?
        .join(&path);
    if !root.is_dir() {
        return Err(kj::failed!("Directory named \"{name}\" not found: {path}"));
    }
    let mut errors = Vec::new();
    let user_worker = if conf.has_user_worker() {
        let reader = conf.get_user_worker().map_err(capnp_error)?;
        let context = format!("Assets service \"{name}\"'s userWorker");
        Some(Designator::from_reader(reader, context, &mut errors).map_err(capnp_error)?)
    } else {
        None
    };
    for error in errors {
        report.service_error(name, error);
    }
    if user_worker.is_some() && (conf.has_compatibility_date() || conf.has_compatibility_flags()) {
        return Err(kj::failed!(
            "Assets service \"{name}\" takes its user Worker's compatibility date and flags"
        ));
    }
    let deployment = Deployment {
        has_user_worker: user_worker.is_some(),
        navigation_prefers_asset_serving: factory.assets_navigation_prefers_asset_serving(index)?,
    };
    let json = Some(text(conf.get_config())?).filter(|json| !json.is_empty());
    let options = serde_json::from_str(json.as_deref().unwrap_or("{}"))
        .map_err(|e| kj::failed!("Assets service \"{name}\"'s config: {e}"))?;
    let warn = &mut |warning| report.service_warning(name, warning);
    let config = Config::new(options, deployment, warn).map_err(|e| kj::failed!("{e}"))?;
    Ok(Rc::new(Assets {
        factory: Rc::clone(factory),
        root,
        config,
        user_worker,
        user: OnceCell::new(),
    }))
}

struct AssetsChannel {
    assets: Rc<Assets>,
    serve_only: bool,
}

impl Channel for AssetsChannel {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let user = self.assets.user.get().filter(|_| !self.serve_only);
        Ok(AssetsRequest {
            assets: Rc::clone(&self.assets),
            user: user.map(|user| (Rc::clone(user), metadata)),
            started: None,
            serve_only: self.serve_only,
        }
        .into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Assets services"))
    }
}

struct AssetsRequest {
    assets: Rc<Assets>,
    /// The user Worker, and the metadata to start the request on it with,
    /// until `started` on it.
    user: Option<(Rc<dyn Channel>, KjOwn<RequestMetadata>)>,
    started: Option<CxxWorkerInterface>,
    serve_only: bool,
}

impl AssetsRequest {
    fn user(&mut self) -> Result<&mut CxxWorkerInterface> {
        if let Some((user, metadata)) = self.user.take() {
            self.started = Some(CxxWorkerInterface::new(user.start_request(metadata)?));
        }
        self.started
            .as_mut()
            .ok_or_else(|| unsupported("Assets services"))
    }
}

#[async_trait::async_trait(?Send)]
impl Service for AssetsRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> ServiceResult {
        let assets = Rc::clone(&self.assets);
        let raw = std::str::from_utf8(url).map_err(|_| kj::failed!("request URL is not UTF-8"))?;
        let parsed = url::Url::parse(raw).map_err(|e| kj::failed!("{e}: {raw}"))?;
        let header = |name| {
            headers
                .get_by_name(name)
                .and_then(|v| std::str::from_utf8(v).ok())
        };
        let request = Request {
            url: &parsed,
            method: match method {
                Method::GET => assets_router::Method::Get,
                Method::HEAD => assets_router::Method::Head,
                _ => assets_router::Method::Other,
            },
            sec_fetch_mode: header("sec-fetch-mode"),
            sec_fetch_dest: header("sec-fetch-dest"),
        };
        let routing = &assets.config.routing;
        let answer = if !self.serve_only && is_cdn_cgi_backslash_bypass(raw, &parsed) {
            Response::new(
                307,
                vec![assets_router::header("Location", parsed.as_str())],
            )
        } else {
            let route = if self.serve_only {
                RuleRoute::AssetWorker
            } else {
                routing.rule_route(parsed.path())
            };
            let exists = |path: &str| assets.stat(path).map(|(_, metadata)| etag(path, &metadata));
            match routing
                .route(route, &request, &exists)
                .map_err(|e| kj::failed!("{e}"))?
            {
                Route::UserWorker {
                    block_image_href: true,
                    ..
                } => Response::blocked(),
                Route::UserWorker {
                    filter_image_response,
                    ..
                } => {
                    let user = self.user()?;
                    if !filter_image_response {
                        return user
                            .request(method, url, headers, request_body, response)
                            .await;
                    }
                    let mut filter = image_filter_response(response.into_ffi());
                    let response = ServiceResponse::from(filter.as_mut());
                    user.request(method, url, headers, request_body, response)
                        .await?;
                    return Ok(filter.finish().await?);
                }
                Route::Respond(answer) => answer,
                // The file went away since routing found it.
                Route::Asset(asset) => match assets.stat(&asset.path) {
                    None => Response::new(404, routing.custom_headers(&request, vec![])),
                    Some((path, metadata)) => {
                        let conditions = Conditions {
                            if_none_match: header("if-none-match"),
                            private: header("authorization").is_some() || header("range").is_some(),
                        };
                        let served = assets.config.serve(
                            &request,
                            conditions,
                            &asset,
                            &etag(&asset.path, &metadata),
                        );
                        // A HEAD gets the length a GET would; a 304 has no body.
                        let length = if served.status == 304 {
                            0
                        } else {
                            metadata.len()
                        };
                        let mut out =
                            assets.send(response, served.status, &served.headers, length)?;
                        if served.body {
                            let mut file =
                                std::fs::File::open(&path).map_err(|e| io_kj_error(&e))?;
                            copy_file(&mut file, 0, length, &mut out).await?;
                        }
                        return Ok(());
                    }
                },
            }
        };
        let length = answer.body.len() as u64;
        let mut out = assets.send(response, answer.status, &answer.headers, length)?;
        Ok(out.write(answer.body.as_bytes()).await?)
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> ServiceResult {
        self.user()?
            .connect(host, headers, connection, response, settings)
            .await
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for AssetsRequest {
    async fn prewarm(&mut self, url: &str) -> worker::Result<()> {
        match self.user() {
            Ok(user) => user.prewarm(url).await,
            Err(_) => Ok(()),
        }
    }

    async fn run_scheduled(
        &mut self,
        time: &SystemTime,
        cron: &str,
    ) -> worker::Result<ScheduledResult> {
        self.user()?.run_scheduled(time, cron).await
    }

    async fn run_alarm(
        &mut self,
        time: &SystemTime,
        retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        self.user()?.run_alarm(time, retry_count).await
    }

    async fn test(&mut self) -> worker::Result<bool> {
        self.user()?.test().await
    }

    async fn custom_event(
        &mut self,
        event: KjOwn<CustomEvent>,
    ) -> worker::Result<CustomEventResult> {
        match self.user() {
            Ok(user) => user.custom_event(event).await,
            Err(_) => worker::not_supported(event).await,
        }
    }
}
