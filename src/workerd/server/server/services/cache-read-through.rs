// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A cache service's `ReadThrough` entrypoint (`CacheDirectory.userWorker` in workerd.capnp):
//! Cloudflare's Workers Caching in front of a Worker, its entries in the service's [`Store`] under
//! keys of their own.
//!
//! A GET or HEAD is answered from a fresh entry of its path and query if there is one
//! (`Cf-Cache-Status: HIT`). Else a GET runs the Worker, and misses of the same key wait for it,
//! then look again. The Worker's response is stored and served from the record (`MISS`) if
//! [`lifetime_of`] allows, else streamed as it comes (`BYPASS`). Other requests, upgrades and
//! ones with `Authorization` go to the Worker untouched. A record keeps the response's
//! `Cache-Tag` and its path for [`Purge`]; neither is served.
//!
//! Not yet: `Vary` (such responses are not stored), `stale-while-revalidate`, `stale-if-error`,
//! `invalidate`, callers' `cf.cacheKey` and `cache: "no-store"`, a cache per entrypoint and
//! version, streaming a miss while it is stored, and one Worker run per key across a cluster's
//! nodes.

use std::cell::Cell;
use std::cell::OnceCell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::io::Write;
use std::pin::Pin;
use std::rc::Rc;
use std::time::Duration;
use std::time::Instant;
use std::time::SystemTime;

use http::HeaderMap;
use http::HeaderName;
use http::HeaderValue;
use http::header;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use tokio::sync::Notify;
use tokio::sync::oneshot;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::CxxWorkerInterface;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::cache_directory;

use super::Cache;
use super::CacheRequest;
use super::Entry;
use super::MAX_ENTRY;
use super::MAX_HEAD;
use super::entry_name;
use super::lifetime;
use super::load;
use super::now_ms;
use super::record_head;
use crate::Result;
use crate::bindings::Designator;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::capnp_error;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::header_table;
use crate::services::not_transferable;

/// What every read-through key starts with, apart from the Cache API's.
pub(super) const PREFIX: &str = "rt-";
/// The header a record keeps its request's path and query in.
const PATH: &str = "cf-cache-path";
/// Headers a record keeps that are not served.
const UNSERVED: [&str; 3] = ["cache-tag", PATH, "cloudflare-cdn-cache-control"];

struct Inner {
    worker: Designator,
    cache: Rc<Cache>,
    linked: OnceCell<Rc<dyn Channel>>,
    /// The keys whose miss is running the Worker, and what waits for it.
    fills: RefCell<HashMap<String, Vec<oneshot::Sender<()>>>>,
}

/// The `ReadThrough` entrypoint of a cache service with a user Worker.
pub(super) struct ReadThrough(Rc<Inner>);

impl ReadThrough {
    /// The entrypoint of cache `name`, over `cache`'s store, if `conf` has a user Worker.
    pub(super) fn new(
        name: &str,
        conf: cache_directory::Reader<'_>,
        cache: &Rc<Cache>,
    ) -> Result<Option<Self>> {
        if !conf.has_user_worker() {
            return Ok(None);
        }
        let mut errors = Vec::new();
        let reader = conf.get_user_worker().map_err(capnp_error)?;
        let context = format!("Cache \"{name}\"'s userWorker");
        let worker = Designator::from_reader(reader, context, &mut errors).map_err(capnp_error)?;
        if let Some(error) = errors.into_iter().next() {
            return Err(kj::failed!("{error}"));
        }
        Ok(Some(Self(Rc::new(Inner {
            worker,
            cache: Rc::clone(cache),
            linked: OnceCell::new(),
            fills: RefCell::default(),
        }))))
    }

    /// Resolves the user Worker, once every service exists.
    pub(super) fn link(&self, lookup: impl FnOnce(&Designator) -> Rc<dyn Channel>) {
        let worker = lookup(&self.0.worker);
        self.0.linked.get_or_init(|| worker);
    }
}

impl Channel for ReadThrough {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let worker = self
            .0
            .linked
            .get()
            .ok_or_else(|| kj::failed!("read-through cache: not linked"))?;
        Ok(Request {
            inner: Rc::clone(&self.0),
            worker: Rc::clone(worker),
            cache: Rc::clone(&self.0.cache),
            metadata: Some(metadata),
            started: None,
        }
        .into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Read-through caches"))
    }
}

/// What `ctx.cache.purge()` removes: entries with any of `tags` in their `Cache-Tag`, or whose
/// path starts with any of `prefixes`, or every one.
#[derive(Clone, Default)]
pub(super) struct Purge {
    tags: Vec<String>,
    prefixes: Vec<String>,
    everything: bool,
}

impl Purge {
    /// From the query of `url`: `tag`, `prefix`, each repeated, and `everything`.
    pub(super) fn parse(url: &[u8]) -> Result<Self> {
        let url = std::str::from_utf8(url).map_err(|_| kj::failed!("purge URL is not UTF-8"))?;
        let url = url::Url::parse(url).map_err(|e| kj::failed!("{e}: {url}"))?;
        let mut purge = Self::default();
        for (name, value) in url.query_pairs() {
            match &*name {
                "tag" => purge.tags.push(value.into_owned()),
                "prefix" => purge.prefixes.push(value.into_owned()),
                "everything" => purge.everything = true,
                _ => {}
            }
        }
        Ok(purge)
    }

    /// The query [`Purge::parse`] reads back.
    pub(super) fn query(&self) -> String {
        let mut query = url::form_urlencoded::Serializer::new(String::new());
        for tag in &self.tags {
            query.append_pair("tag", tag);
        }
        for prefix in &self.prefixes {
            query.append_pair("prefix", prefix);
        }
        if self.everything {
            query.append_key_only("everything");
        }
        query.finish()
    }

    /// Whether a record whose headers are `headers` goes.
    pub(super) fn matches(&self, headers: &HeaderMap) -> bool {
        let text = |name| headers.get(name).and_then(|v| v.to_str().ok());
        let path = text(PATH).unwrap_or_default();
        self.everything
            || text("cache-tag").unwrap_or_default().split(',').any(|t| {
                self.tags
                    .iter()
                    .any(|tag| tag.eq_ignore_ascii_case(t.trim()))
            })
            || self.prefixes.iter().any(|p| path.starts_with(p.as_str()))
    }
}

/// How long a purge is remembered for the fills begun before it. One older stores nothing.
const KEEP: Duration = Duration::from_mins(10);

/// The purges begun on this node, which a fill begun before one checks before it stores, and the
/// fills storing. So once [`Purges::fence`] returns, no fill begun before stores what it purges.
#[derive(Default)]
pub(super) struct Purges {
    begun: Cell<u64>,
    /// The purges begun within `KEEP`, by number.
    recent: RefCell<VecDeque<(u64, Instant, Purge)>>,
    /// The number of the latest purge no longer in `recent`.
    forgot: Cell<u64>,
    /// The fills storing, by how many purges had begun when each began.
    storing: RefCell<BTreeMap<u64, usize>>,
    stored: Notify,
}

/// When a fill began: after how many purges, and whether its node was active.
pub(super) struct Ticket {
    after: u64,
    active: bool,
}

/// A fill storing, until dropped.
pub(super) struct Storing(Rc<Purges>, u64);

impl Purges {
    pub(super) fn ticket(&self, active: bool) -> Ticket {
        let after = self.begun.get();
        Ticket { after, active }
    }

    /// Lets the fill of `ticket` store a record with `headers`, unless its node was not active
    /// as it began, so may be in no purger's view, or a purge begun since matches them.
    pub(super) fn store(self: &Rc<Self>, ticket: &Ticket, headers: &HeaderMap) -> Option<Storing> {
        let after = ticket.after;
        let recent = self.recent.borrow();
        let mut since = recent.iter().filter(|(n, ..)| *n > after);
        if !ticket.active || after < self.forgot.get() || since.any(|(.., p)| p.matches(headers)) {
            return None;
        }
        *self.storing.borrow_mut().entry(after).or_default() += 1;
        Some(Storing(Rc::clone(self), after))
    }

    /// Begins `purge`, and waits for the fills begun before it that are storing.
    pub(super) async fn fence(&self, purge: &Purge) {
        let n = self.begun.get() + 1;
        self.begun.set(n);
        {
            let mut recent = self.recent.borrow_mut();
            while let Some((old, ..)) = recent.pop_front_if(|(_, at, _)| at.elapsed() > KEEP) {
                self.forgot.set(old);
            }
            recent.push_back((n, Instant::now(), purge.clone()));
        }
        loop {
            let stored = self.stored.notified();
            let storing = self
                .storing
                .borrow()
                .first_key_value()
                .map(|(after, _)| *after);
            if storing.is_none_or(|after| after >= n) {
                return;
            }
            stored.await;
        }
    }
}

impl Drop for Storing {
    fn drop(&mut self) {
        let mut storing = self.0.storing.borrow_mut();
        if let Some(count) = storing.get_mut(&self.1) {
            *count -= 1;
            if *count == 0 {
                storing.remove(&self.1);
            }
        }
        self.0.stored.notify_waiters();
    }
}

/// How long Workers Caching keeps a response: by `Cloudflare-CDN-Cache-Control`, else
/// `CDN-Cache-Control`, else as the Cache API does; `None` if not at all. `headers` lose what
/// [`lifetime`] drops.
fn lifetime_of(status: u16, headers: &mut HeaderMap, now: u64) -> Option<u64> {
    let never = matches!(status, 206 | 304 | 520..=526) || headers.contains_key(header::VARY);
    let names = ["cloudflare-cdn-cache-control", "cdn-cache-control"];
    let Some(cdn) = names.iter().find_map(|n| headers.get(*n)).cloned() else {
        return lifetime(status, headers, false, now).filter(|&ttl| ttl > 0 && !never);
    };
    let own = headers.insert(header::CACHE_CONTROL, cdn);
    let ttl = lifetime(status, headers, false, now);
    match own {
        Some(own) => headers.insert(header::CACHE_CONTROL, own),
        None => headers.remove(header::CACHE_CONTROL),
    };
    ttl.filter(|&ttl| ttl > 0 && !never)
}

/// The miss of a key that runs the Worker: once it ends, what waits for it looks again.
struct Fill<'a>(
    &'a RefCell<HashMap<String, Vec<oneshot::Sender<()>>>>,
    String,
);

impl Drop for Fill<'_> {
    fn drop(&mut self) {
        for waiter in self.0.borrow_mut().remove(&self.1).unwrap_or_default() {
            let _ = waiter.send(());
        }
    }
}

struct Request {
    inner: Rc<Inner>,
    worker: Rc<dyn Channel>,
    cache: Rc<Cache>,
    /// The metadata to start the request on the Worker with, until it is.
    metadata: Option<KjOwn<RequestMetadata>>,
    started: Option<CxxWorkerInterface>,
}

impl Request {
    fn worker(&mut self) -> Result<&mut CxxWorkerInterface> {
        if let Some(metadata) = self.metadata.take() {
            self.started = Some(CxxWorkerInterface::new(
                self.worker.start_request(metadata)?,
            ));
        }
        self.started
            .as_mut()
            .ok_or_else(|| kj::failed!("read-through cache: the Worker was asked already"))
    }

    /// Runs the Worker for a miss of `key`, `path`, and answers with what it says.
    async fn fill(
        &mut self,
        fill: Fill<'_>,
        key: &str,
        path: &str,
        url: &str,
        request: HeadersRef<'_>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let metadata = self
            .metadata
            .take()
            .ok_or_else(|| kj::failed!("read-through cache: the Worker was asked already"))?;
        let ticket = self.cache.purges.ticket(self.cache.store.active());
        let interface = self.worker.start_request(metadata)?;
        let call = ffi::http_call(interface, Method::GET, url, request.as_ffi(), 0);
        let mut answer = ffi::http_call_response(call, "").await?;
        let mut headers = HeaderMap::new();
        for pair in answer.headers.chunks(2) {
            let name = HeaderName::from_bytes(pair[0].as_bytes());
            if let (Ok(name), Ok(value)) = (name, HeaderValue::from_str(&pair[1])) {
                headers.append(name, value);
            }
        }
        let status = u16::try_from(answer.status).unwrap_or(500);
        let now = now_ms();
        let served = CacheRequest {
            cache: Rc::clone(&self.cache),
            key: None,
            from: None,
        };
        let Some(ttl) = lifetime_of(status, &mut headers, now) else {
            drop(fill);
            for name in UNSERVED {
                headers.remove(name);
            }
            headers.insert("cf-cache-status", HeaderValue::from_static("BYPASS"));
            let length = headers
                .get(header::CONTENT_LENGTH)
                .and_then(|v| v.to_str().ok()?.parse().ok());
            let kj = kj_headers(header_table(&self.cache.factory), &headers)?;
            let text = &answer.status_text;
            let mut out = response.send(status.into(), text, HeadersRef::from(&*kj), length)?;
            let mut chunk = vec![0; 64 * 1024];
            loop {
                let read = answer.body.as_mut().try_read(&mut chunk, 1).await?;
                if read == 0 {
                    return Ok(());
                }
                out.write(&chunk[..read]).await?;
            }
        };
        let (temp, mut draft) = self.cache.store.draft(key)?;
        let mut stored = headers.clone();
        stored.insert(PATH, header_value(path)?);
        let head = record_head(
            now.saturating_add(ttl),
            status,
            &answer.status_text,
            &stored,
        );
        let io = |e: std::io::Error| io_kj_error(&e);
        draft.write_all(&head).map_err(io)?;
        let (mut size, mut chunk) = (0, vec![0; 64 * 1024]);
        loop {
            let read = answer.body.as_mut().try_read(&mut chunk, 1).await?;
            if read == 0 {
                break;
            }
            draft.write_all(&chunk[..read]).map_err(io)?;
            size += read as u64;
        }
        let copy = temp.open()?;
        let storing = (size <= MAX_ENTRY && head.len() <= MAX_HEAD)
            .then(|| self.cache.purges.store(&ticket, &stored))
            .flatten();
        if let Some(_storing) = storing {
            self.cache.store.put(key, temp, draft).await?;
        }
        drop(fill);
        for name in UNSERVED {
            headers.remove(name);
        }
        let entry = Entry {
            file: copy,
            status,
            text: answer.status_text,
            headers,
            start: head.len() as u64,
            size,
        };
        served.serve(entry, "MISS", request, response).await
    }
}

#[async_trait::async_trait(?Send)]
impl Service for Request {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let bypass = !matches!(method, Method::GET | Method::HEAD)
            || headers.get_by_name("Upgrade").is_some()
            || headers.get_by_name("Authorization").is_some();
        if bypass {
            return self
                .worker()?
                .request(method, url, headers, body, response)
                .await;
        }
        let raw = std::str::from_utf8(url).map_err(|_| kj::failed!("request URL is not UTF-8"))?;
        let parsed = url::Url::parse(raw).map_err(|e| kj::failed!("{e}: {raw}"))?;
        let path = match parsed.query() {
            Some(query) => format!("{}?{query}", parsed.path()),
            None => parsed.path().to_owned(),
        };
        let key = format!("{PREFIX}{}", entry_name(b"", path.as_bytes()));
        let inner = Rc::clone(&self.inner);
        loop {
            if let Some(mut entry) = load(&*self.cache.store, &key, now_ms()).await? {
                for name in UNSERVED {
                    entry.headers.remove(name);
                }
                let served = CacheRequest {
                    cache: Rc::clone(&self.cache),
                    key: None,
                    from: None,
                };
                return served.serve(entry, "HIT", headers, response).await;
            }
            if matches!(method, Method::HEAD) {
                return self
                    .worker()?
                    .request(method, url, headers, body, response)
                    .await;
            }
            let wait = {
                let mut fills = inner.fills.borrow_mut();
                if let Some(waiting) = fills.get_mut(&key) {
                    let (waiter, done) = oneshot::channel();
                    waiting.push(waiter);
                    Some(done)
                } else {
                    fills.insert(key.clone(), Vec::new());
                    None
                }
            };
            let Some(done) = wait else {
                break;
            };
            let _ = done.await;
        }
        let fill = Fill(&inner.fills, key.clone());
        self.fill(fill, &key, &path, raw, headers, response).await
    }

    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Result<()> {
        self.worker()?
            .connect(host, headers, connection, response, settings)
            .await
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for Request {
    async fn prewarm(&mut self, url: &str) -> Result<()> {
        self.worker()?.prewarm(url).await
    }

    async fn run_scheduled(&mut self, time: &SystemTime, cron: &str) -> Result<ScheduledResult> {
        self.worker()?.run_scheduled(time, cron).await
    }

    async fn run_alarm(&mut self, time: &SystemTime, retry_count: u32) -> Result<AlarmResult> {
        self.worker()?.run_alarm(time, retry_count).await
    }

    async fn test(&mut self) -> Result<bool> {
        self.worker()?.test().await
    }

    async fn custom_event(&mut self, event: KjOwn<CustomEvent>) -> Result<CustomEventResult> {
        self.worker()?.custom_event(event).await
    }
}
