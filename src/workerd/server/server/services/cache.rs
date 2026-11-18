// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `cache` service (`CacheDirectory` in workerd.capnp): the store of `/workerd/cache.js`.
//!
//! In a cluster it is one across the nodes, by the protocol in `cache-cluster.rs`; on one
//! machine that Worker stores to a writable `disk` instead. A record is opaque, with an expiry:
//!
//! - `GET <url>`: 200 with the record and its `Content-Length`, or 404, as once it expired.
//! - `PUT <url>` with `X-Expires: <ms since the epoch>`: 204 stored, 413 refused, 500 if the
//!   outcome is unknown.
//! - `DELETE <url>`: 204 had an unexpired record, 404 did not, 500 if unknown.
//!
//! The URL's last segment is the key. A value is a file: its expiry and a newline, then the
//! record. A put writes a draft that the store takes once the whole body was read; a request
//! cancelled first stores nothing. From a worker in this process, a body without a length that
//! fails ends instead, so the worker cancels. Disk I/O is synchronous, as in `disk`.

use std::fs::File;
use std::io::Read;
use std::io::Write;
use std::pin::Pin;
use std::rc::Rc;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use http::HeaderMap;
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
use worker::AlarmResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::cache_directory;

use crate::Result;
use crate::bridge::ffi;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::text;
use crate::services::disk::copy_file;
use crate::services::disk::kj_headers;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::unsupported;

/// A value's expiry, in ms since the epoch, and where its record starts.
fn expiry(file: &mut File) -> Result<Option<(u64, u64)>> {
    let mut line = [0; 24];
    let read = file.read(&mut line).map_err(|e| io_kj_error(&e))?;
    let Some(end) = line[..read].iter().position(|&b| b == b'\n') else {
        return Ok(None);
    };
    let expires = std::str::from_utf8(&line[..end]).ok();
    Ok(expires
        .and_then(|e| e.parse().ok())
        .map(|e| (e, end as u64 + 1)))
}

/// Whether the value in `file` is unexpired at `now`.
fn fresh(file: &mut File, now: u64) -> Result<bool> {
    Ok(expiry(file)?.is_some_and(|(e, _)| e > now))
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

struct Cache {
    factory: Rc<Factory>,
    store: clustered::Clustered,
}

/// A `cache` service: the nodes' store of `/workerd/cache.js`.
pub struct CacheService(Rc<Cache>);

/// `path_override` is the CLI's `--directory-path` for this service.
pub fn make_cache_service(
    name: &str,
    conf: cache_directory::Reader<'_>,
    path_override: Option<&str>,
    factory: &Rc<Factory>,
    cluster: Option<&Rc<ClusterNode>>,
) -> Result<Rc<dyn Channel>> {
    if !factory.experimental() {
        return Err(kj::failed!(
            "Cache services are an experimental feature which may change or go away in the \
             future. You must run workerd with `--experimental` to use this feature."
        ));
    }
    let Some(node) = cluster else {
        return Err(kj::failed!(
            "Cache \"{name}\" requires Config.cluster. On one machine, /workerd/cache.js stores \
             to a writable disk."
        ));
    };
    let path = match path_override {
        Some(path) => path.to_owned(),
        None if conf.has_path() => text(conf.get_path())?,
        None => {
            return Err(kj::failed!(
                "Cache \"{name}\" has no path in the config, so must be specified on the \
                 command line with `--directory-path`."
            ));
        }
    };
    let root = std::env::current_dir()
        .map_err(|e| io_kj_error(&e))?
        .join(&path);
    if !root.is_dir() {
        return Err(kj::failed!(
            "Directory of cache \"{name}\" not found: {path}"
        ));
    }
    let store = clustered::store(name, &root, node, factory)?;
    let factory = Rc::clone(factory);
    Ok(Rc::new(CacheService(Rc::new(Cache { factory, store }))))
}

impl Channel for CacheService {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let from = ffi::request_metadata_forwarded_by(&metadata).into();
        let cache = Rc::clone(&self.0);
        Ok(CacheRequest { cache, from }.into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Cache services"))
    }
}

/// Reads `body` to its end.
async fn drain(mut body: Pin<&mut AsyncInputStream>) -> Result<()> {
    let mut chunk = vec![0; 64 * 1024];
    while body.as_mut().try_read(&mut chunk, 1).await? > 0 {}
    Ok(())
}

/// One request to a [`CacheService`], or a message from cluster node `from`.
struct CacheRequest {
    cache: Rc<Cache>,
    from: Option<String>,
}

impl CacheRequest {
    /// Answers `status` with `headers` and `length` bytes to come.
    fn send<'a>(
        &self,
        response: ServiceResponse<'a>,
        status: u32,
        text: &str,
        headers: &HeaderMap,
        length: u64,
    ) -> Result<kj::io::AsyncOutputStream<'a>> {
        let headers = kj_headers(header_table(&self.cache.factory), headers)?;
        response.send(status, text, HeadersRef::from(&*headers), Some(length))
    }

    fn answer(&self, response: ServiceResponse<'_>, status: u32, text: &str) -> Result<()> {
        self.send(response, status, text, &HeaderMap::new(), 0)
            .map(drop)
    }

    /// Answers a write that was done, not (`not`), or whose outcome is unknown.
    fn wrote(
        &self,
        response: ServiceResponse<'_>,
        done: Option<bool>,
        not: (u32, &str),
    ) -> Result<()> {
        match done {
            Some(true) => self.answer(response, 204, "No Content"),
            Some(false) => self.answer(response, not.0, not.1),
            None => self.answer(response, 500, "Internal Server Error"),
        }
    }

    async fn get(&self, key: &str, response: ServiceResponse<'_>) -> Result<()> {
        let store = &self.cache.store;
        let Some(mut file) = store.get(key).await? else {
            return self.answer(response, 404, "Not Found");
        };
        // A near copy, leased, may expire before its home drops it.
        let Some((_, start)) = expiry(&mut file)?.filter(|&(e, _)| e > now_ms()) else {
            store.expire(key);
            return self.answer(response, 404, "Not Found");
        };
        let size = file.metadata().map_err(|e| io_kj_error(&e))?.len() - start;
        let mut headers = HeaderMap::new();
        headers.insert(header::CONTENT_LENGTH, HeaderValue::from(size));
        let mut out = self.send(response, 200, "OK", &headers, size)?;
        copy_file(&mut file, start, size, &mut out).await
    }

    async fn put(
        &self,
        key: &str,
        headers: HeadersRef<'_>,
        mut body: Pin<&mut AsyncInputStream>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let expires = headers
            .get_by_name("X-Expires")
            .and_then(|v| std::str::from_utf8(v).ok()?.parse::<u64>().ok());
        let Some(expires) = expires else {
            drain(body).await?;
            return self.answer(response, 400, "Bad Request");
        };
        let (temp, mut file) = self.cache.store.draft()?;
        let mut write = |bytes: &[u8]| file.write_all(bytes).map_err(|e| io_kj_error(&e));
        write(format!("{expires}\n").as_bytes())?;
        let mut chunk = vec![0; 64 * 1024];
        loop {
            let read = body.as_mut().try_read(&mut chunk, 1).await?;
            if read == 0 {
                break;
            }
            write(&chunk[..read])?;
        }
        let stored = self.cache.store.put(key, temp, file).await;
        self.wrote(response, stored, (413, "Payload Too Large"))
    }
}

#[async_trait::async_trait(?Send)]
impl Service for CacheRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let store = &self.cache.store;
        if let Some(from) = &self.from {
            return store
                .message(from, method, url, headers, body, response)
                .await;
        }
        let url = String::from_utf8_lossy(url);
        let key = url.rsplit('/').next().unwrap_or_default();
        match method {
            Method::GET => self.get(key, response).await,
            Method::PUT => self.put(key, headers, body, response).await,
            Method::DELETE => self.wrote(response, store.delete(key).await, (404, "Not Found")),
            _ => {
                drain(body).await?;
                self.answer(response, 405, "Method Not Allowed")
            }
        }
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(unsupported("Cache services"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for CacheRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("Cache services"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("Cache services"))
    }
}

#[path = "cache-cluster.rs"]
mod clustered;

#[cfg(test)]
#[path = "cache-test.rs"]
mod tests;
