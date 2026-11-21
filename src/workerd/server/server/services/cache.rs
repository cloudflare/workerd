// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `cache` service (`CacheDirectory` in workerd.capnp): the Cache API's backend, on the
//! protocol workerd's `caches` speak to a Worker's `cacheApiOutbound` (`api/cache.c++`):
//!
//! - `GET <url>` matches, with `Cache-Control: only-if-cached`: a hit is any response with
//!   `CF-Cache-Status: HIT`, a miss a 504 with `CF-Cache-Status: MISS`.
//! - `PUT <url>` puts: the body is the response to store, serialized as HTTP/1.1. 204 stored,
//!   413 not.
//! - `PURGE <url>` deletes: 200 deleted, 404 not found.
//!
//! `caches.open(name)` sends `CF-Cache-Namespace: <name, URI-encoded>`; the default cache sends
//! none, and is disjoint from every named one. A request's `cf.cacheKey`, when set, is its key
//! instead of its URL.
//!
//! Whether a response is stored, and for how long, follows Miniflare (see [`lifetime`]). A
//! match answers one or several byte ranges, and `If-None-Match` or `If-Modified-Since` with a
//! 304. An expired entry is a miss, and is removed when read.
//!
//! What entries mean is decided here; where they live is a [`Store`]'s: a [`Directory`], or
//! with `Config.cluster` the nodes' one cache (`cache-cluster.rs`). An entry is a record, a
//! file: a line with its expiry in ms since the epoch, then the response as HTTP/1.1, its body
//! as it was put, still content-encoded. Its key is its cache's directory and its file
//! ([`entry_name`]), so that one cache can be removed by itself. A put writes a draft and has
//! the store put it once the whole body was read and matched its `Content-Length`, so an entry
//! is never partial. Disk I/O is synchronous, as in `disk`.

use std::collections::HashMap;
use std::fs::File;
use std::io::Read;
use std::io::Write;
use std::path::PathBuf;
use std::pin::Pin;
use std::rc::Rc;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use data_encoding::HEXLOWER;
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
use kj::http::ServiceResult;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj::io::AsyncOutputStream;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use percent_encoding::percent_decode;
use ring::digest::SHA256;
use ring::digest::digest;
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
use crate::services::cf_blob_json;
use crate::services::disk::Temporary;
use crate::services::disk::copy_file;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::unsupported;

/// The largest body stored, as production's default `cache_max_file_size`.
const MAX_ENTRY: u64 = 512 * 1024 * 1024;
/// The largest response head read.
const MAX_HEAD: usize = 1024 * 1024;
/// The statuses http-cache-semantics stores, and those it stores without an explicit lifetime.
const KNOWN: [u16; 15] = [
    200, 203, 204, 206, 300, 301, 302, 303, 307, 308, 404, 405, 410, 414, 501,
];
const BY_DEFAULT: [u16; 10] = [200, 203, 204, 300, 301, 404, 405, 410, 414, 501];
const HOP_BY_HOP: [&str; 8] = [
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
];
const DAY_MS: u64 = 24 * 3600 * 1000;

// =======================================================================================
// Pure parts

/// The path of the entry for `key` in the cache `namespace` names (`CF-Cache-Namespace` as sent:
/// the name URI-encoded, empty for `caches.default`): `<cache>/<key>`, each the SHA-256 in
/// lowercase hex, but `default` for the default cache.
fn entry_name(namespace: &[u8], key: &[u8]) -> String {
    let hex = |b: &[u8]| HEXLOWER.encode(digest(&SHA256, b).as_ref());
    let cache = match namespace {
        b"" => "default".to_owned(),
        name => hex(&percent_decode(name).collect::<Vec<_>>()),
    };
    format!("{cache}/{}", hex(key))
}

/// A request's `cf.cacheKey`, from its cf blob.
fn cache_key(cf: &str) -> Option<String> {
    let cf: serde_json::Value = serde_json::from_str(cf).ok()?;
    let key = cf.get("cacheKey")?.as_str()?;
    (!key.is_empty()).then(|| key.to_owned())
}

/// Where the blank line that ends a head starts in `buf`, whose first `from` bytes were
/// searched already.
fn blank_line(buf: &[u8], from: usize) -> Option<usize> {
    let from = from.saturating_sub(3);
    let at = buf.get(from..)?.windows(4).position(|w| w == b"\r\n\r\n")?;
    Some(from + at)
}

/// A response head without its blank line: the status, its text and the headers.
fn parse_head(head: &[u8]) -> Option<(u16, String, HeaderMap)> {
    let mut lines = head
        .split(|&b| b == b'\n')
        .map(|line| line.strip_suffix(b"\r").unwrap_or(line));
    let line = std::str::from_utf8(lines.next()?).ok()?;
    let (_, rest) = line.strip_prefix("HTTP/")?.split_once(' ')?;
    let (code, text) = rest.split_once(' ').unwrap_or((rest, ""));
    let status = code.parse().ok().filter(|s| (100..1000).contains(s))?;
    let mut headers = HeaderMap::new();
    for line in lines {
        let colon = line.iter().position(|&b| b == b':')?;
        let value = HeaderValue::from_bytes(line[colon + 1..].trim_ascii()).ok()?;
        headers.append(HeaderName::from_bytes(&line[..colon]).ok()?, value);
    }
    Some((status, text.to_owned(), headers))
}

/// An entry's file up to its body: the expiry line, the head and the blank line.
fn record_head(expires: u64, status: u16, text: &str, headers: &HeaderMap) -> Vec<u8> {
    let mut out = format!("{expires}\r\nHTTP/1.1 {status} {text}\r\n").into_bytes();
    for (name, value) in headers {
        out.extend([name.as_str().as_bytes(), b": ", value.as_bytes(), b"\r\n"].concat());
    }
    out.extend(b"\r\n");
    out
}

/// The response an entry's head (without its blank line) stores, if it is fresh at `now`, in
/// ms since the epoch.
fn parse_record(head: &[u8], now: u64) -> Option<(u16, String, HeaderMap)> {
    let newline = head.iter().position(|&b| b == b'\n')?;
    if expiry(head)? <= now {
        return None;
    }
    parse_head(&head[newline + 1..])
}

/// A record's expiry, from its first line.
fn expiry(record: &[u8]) -> Option<u64> {
    let line = record.split(|&b| b == b'\n').next()?;
    std::str::from_utf8(line).ok()?.trim_end().parse().ok()
}

/// Whether the record in `file` is fresh at `now`.
fn fresh(file: &mut File, now: u64) -> Result<bool> {
    let mut line = [0; 24];
    let read = file.read(&mut line).map_err(|e| io_kj_error(&e))?;
    Ok(expiry(&line[..read]).is_some_and(|e| e > now))
}

/// A `Range` header's inclusive ranges of a body of `size` bytes, as Miniflare's
/// `shared/range.ts` parses it: none for the whole body, `None` if it is unsatisfiable.
fn parse_ranges(header: &str, size: u64) -> Option<Vec<(u64, u64)>> {
    let (unit, spec) = header.split_once('=')?;
    if !unit.trim_matches(' ').eq_ignore_ascii_case("bytes") {
        return None;
    }
    let bound = |s: &str| match s.trim_matches(' ') {
        "" => Some(None),
        s if s.bytes().all(|b| b.is_ascii_digit()) => s.parse().ok().map(Some),
        _ => None,
    };
    let mut out = Vec::new();
    if spec.trim_start().is_empty() {
        return Some(out);
    }
    for part in spec.split(',') {
        let (first, last) = part.split_once('-')?;
        match (bound(first)?, bound(last)?) {
            (Some(first), last) => {
                if first >= size || last.is_some_and(|last| first > last) {
                    return None;
                }
                out.push((first, last.map_or(size - 1, |last| last.min(size - 1))));
            }
            (None, None) => return None,
            (None, Some(n)) if n >= size => return Some(Vec::new()),
            (None, Some(0)) => {}
            (None, Some(n)) => out.push((size - n, size - 1)),
        }
    }
    Some(out)
}

/// The opaque part of an entity tag, weak or strong.
fn entity_tag(value: &str) -> Option<&str> {
    let value = value.trim();
    let value = value.strip_prefix("W/").unwrap_or(value);
    let tag = value.strip_prefix('"')?.strip_suffix('"')?;
    (!tag.is_empty()).then_some(tag)
}

fn date_ms(value: &str) -> Option<u64> {
    let date = httpdate::parse_http_date(value).ok()?;
    Some(date.duration_since(UNIX_EPOCH).ok()?.as_millis() as u64)
}

/// Whether `If-None-Match` (weakly) or `If-Modified-Since` makes a match of a response with
/// `headers` a 304, as Miniflare decides.
fn not_modified(
    if_none_match: Option<&str>,
    if_modified_since: Option<&str>,
    headers: &HeaderMap,
) -> bool {
    let etag = text_of(headers, header::ETAG).and_then(entity_tag);
    if let (Some(inm), Some(etag)) = (if_none_match, etag)
        && (inm.trim() == "*" || inm.split(',').any(|t| entity_tag(t) == Some(etag)))
    {
        return true;
    }
    let modified = text_of(headers, header::LAST_MODIFIED).and_then(date_ms);
    matches!((modified, if_modified_since.and_then(date_ms)), (Some(m), Some(s)) if m <= s)
}

/// The first value of `name`, unless it is empty or not text.
fn text_of(headers: &HeaderMap, name: HeaderName) -> Option<&str> {
    let value = headers.get(name)?.to_str().ok()?;
    (!value.is_empty()).then_some(value)
}

/// A delta-seconds value, read as JavaScript's `parseInt` reads it, in ms; 0 if it has no
/// digits.
fn delta_ms(value: Option<&str>) -> u64 {
    let value = value.unwrap_or_default().trim_start();
    let digits = value
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(value.len());
    value[..digits]
        .parse()
        .map_or(0, |s: u64| s.saturating_mul(1000))
}

/// A `Cache-Control` directive: its name, lowercase, and its value, unquoted.
fn directive(part: &str) -> (String, Option<&str>) {
    let (name, value) = part.split_once('=').map_or((part, None), |(name, value)| {
        (name, Some(value.trim().trim_matches('"')))
    });
    (name.trim().to_ascii_lowercase(), value)
}

/// How long, in ms, a response with `status` and `headers` stays fresh when put at `now`, in ms
/// since the epoch; `None` if it must not be stored. `authorization` is whether the put had an
/// `Authorization` header. `headers` become those to store: hop-by-hop ones are dropped, and
/// `private=set-cookie` drops `Set-Cookie`.
///
/// This is what Miniflare asks of http-cache-semantics 4.1.1, as a shared cache that ignores the
/// request's `Cache-Control`. Not stored: `no-store`, `private`, `Set-Cookie`, an
/// `Authorization` without `public`, `s-maxage` or `must-revalidate`, a status it does not know,
/// or one it does not store by default and that has no freshness information. The lifetime is
/// `s-maxage`, else `max-age`, else `Expires` minus `Date`, else a tenth of the time since
/// `Last-Modified`, less `Age`; `immutable` makes it at least a day, `no-cache` 0.
fn lifetime(status: u16, headers: &mut HeaderMap, authorization: bool, now: u64) -> Option<u64> {
    let cache_control = headers
        .get(header::CACHE_CONTROL)
        .map(|v| String::from_utf8_lossy(v.as_bytes()).into_owned());
    let mut cc: HashMap<_, _> = cache_control
        .as_deref()
        .unwrap_or_default()
        .split(',')
        .map(directive)
        .filter(|(name, _)| !name.is_empty())
        .collect();
    if cache_control.is_none()
        && text_of(headers, header::PRAGMA).is_some_and(|p| p.contains("no-cache"))
    {
        cc.insert("no-cache".to_owned(), None);
    }
    let connection = text_of(headers, header::CONNECTION).map(str::to_ascii_lowercase);
    for name in HOP_BY_HOP
        .into_iter()
        .chain(connection.iter().flat_map(|c| c.split(',').map(str::trim)))
    {
        headers.remove(name);
    }
    if cc.get("private") == Some(&Some("set-cookie")) {
        cc.remove("private");
        headers.remove(header::SET_COOKIE);
        let kept = cache_control
            .as_deref()
            .unwrap_or_default()
            .split(',')
            .filter(|part| directive(part) != ("private".to_owned(), Some("set-cookie")))
            .collect::<Vec<_>>()
            .join(",");
        match HeaderValue::from_str(kept.trim_matches(|c: char| c.is_whitespace() || c == ',')) {
            Ok(value) if !value.is_empty() => headers.insert(header::CACHE_CONTROL, value),
            _ => headers.remove(header::CACHE_CONTROL),
        };
    }
    let on = |name: &str| {
        cc.get(name)
            .is_some_and(|v| v.is_none_or(|v| !v.is_empty()))
    };
    let expires = text_of(headers, header::EXPIRES);
    let storable = KNOWN.contains(&status)
        && !on("no-store")
        && !on("private")
        && !headers.contains_key(header::SET_COOKIE)
        && (!authorization || on("must-revalidate") || on("public") || on("s-maxage"))
        && (expires.is_some()
            || on("max-age")
            || on("s-maxage")
            || on("public")
            || BY_DEFAULT.contains(&status));
    if !storable {
        return None;
    }
    let age = delta_ms(text_of(headers, header::AGE));
    let fresh = |ms: u64| ms.saturating_sub(age);
    if on("no-cache") || on("proxy-revalidate") {
        return Some(0);
    }
    if let Some(name) = ["s-maxage", "max-age"].into_iter().find(|name| on(name)) {
        return Some(fresh(delta_ms(cc[name])));
    }
    let min = if on("immutable") { DAY_MS } else { 0 };
    let served = text_of(headers, header::DATE)
        .and_then(date_ms)
        .unwrap_or(now);
    if let Some(expires) = expires {
        let expires = date_ms(expires).filter(|&e| e >= served);
        return Some(expires.map_or(0, |e| fresh(min.max(e - served))));
    }
    let modified = text_of(headers, header::LAST_MODIFIED).and_then(date_ms);
    let heuristic = modified
        .filter(|&m| served > m)
        .map_or(0, |m| (served - m) / 10);
    Some(fresh(min.max(heuristic)))
}

// =======================================================================================
// Where records live

/// Where records live, apart from what they mean: a record is a file on local disk, and a key
/// an [`entry_name`]. `None` is an outcome that is unknown, as a remote store's can be.
#[async_trait::async_trait(?Send)]
trait Store {
    /// The record of `key`, if there is one.
    async fn get(&self, key: &str) -> Result<Option<File>>;

    /// A file to write a record of `key` to, removed unless [`Store::put`] stores it.
    fn draft(&self, key: &str) -> Result<(Temporary, File)>;

    /// Makes the record written to `file` that of `key`: whether it was stored.
    async fn put(&self, key: &str, temp: Temporary, file: File) -> Result<Option<bool>>;

    /// Deletes the record of `key`: whether there was one fresh at `now`.
    async fn delete(&self, key: &str, now: u64) -> Result<Option<bool>>;

    /// Drops a record of `key` found expired or corrupt. Not a write: what any reader may do.
    async fn expire(&self, key: &str) -> Result<()>;

    /// A message from cluster node `from`, a request that names it as `forwardedBy`.
    async fn message(
        &self,
        from: &str,
        _method: Method,
        _url: &[u8],
        _headers: HeadersRef<'_>,
        _body: Pin<&mut AsyncInputStream>,
        _response: ServiceResponse<'_>,
    ) -> Result<()> {
        Err(kj::failed!(
            "a cache outside a cluster got a message from {from}"
        ))
    }
}

/// The records in a directory, each the file its key names.
struct Directory(PathBuf);

/// `None` for a file that is not there.
fn found<T>(result: std::io::Result<T>) -> Result<Option<T>> {
    match result {
        Ok(t) => Ok(Some(t)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(io_kj_error(&e)),
    }
}

#[async_trait::async_trait(?Send)]
impl Store for Directory {
    async fn get(&self, key: &str) -> Result<Option<File>> {
        found(File::open(self.0.join(key)))
    }

    fn draft(&self, key: &str) -> Result<(Temporary, File)> {
        let path = self.0.join(key);
        if let Some(cache) = path.parent() {
            std::fs::create_dir_all(cache).map_err(|e| io_kj_error(&e))?;
        }
        Temporary::beside(&path)
    }

    async fn put(&self, key: &str, temp: Temporary, file: File) -> Result<Option<bool>> {
        temp.rename(file, &self.0.join(key))?;
        Ok(Some(true))
    }

    async fn delete(&self, key: &str, now: u64) -> Result<Option<bool>> {
        let path = self.0.join(key);
        let Some(mut file) = found(File::open(&path))? else {
            return Ok(Some(false));
        };
        let fresh = fresh(&mut file, now)?;
        drop(file);
        let deleted = found(std::fs::remove_file(path))?;
        Ok(Some(deleted.is_some() && fresh))
    }

    async fn expire(&self, key: &str) -> Result<()> {
        found(std::fs::remove_file(self.0.join(key))).map(drop)
    }
}

// =======================================================================================
// The service

struct Cache {
    factory: Rc<Factory>,
    store: Box<dyn Store>,
}

/// A `cache` service: the Cache API's backend, its entries in a directory, or shared by the
/// nodes of a cluster.
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
    let store: Box<dyn Store> = match cluster {
        Some(node) => Box::new(clustered::store(name, &root, node, factory)?),
        None => Box::new(Directory(root)),
    };
    let factory = Rc::clone(factory);
    Ok(Rc::new(CacheService(Rc::new(Cache { factory, store }))))
}

impl Channel for CacheService {
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        let key = cf_blob_json(&metadata).and_then(|cf| cache_key(&cf));
        let from = ffi::request_metadata_forwarded_by(&metadata).into();
        let cache = Rc::clone(&self.0);
        Ok(CacheRequest { cache, key, from }.into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Cache services"))
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

/// An entry: the response stored, whose body is `size` bytes of `file` from `start`.
struct Entry {
    file: File,
    status: u16,
    text: String,
    headers: HeaderMap,
    start: u64,
    size: u64,
}

/// The entry of `key` in `store`, if it is fresh at `now`. One that expired, or that is not a
/// record, is deleted.
async fn load(store: &dyn Store, key: &str, now: u64) -> Result<Option<Entry>> {
    let Some(mut file) = store.get(key).await? else {
        return Ok(None);
    };
    let mut head = Vec::new();
    let mut chunk = [0; 4096];
    let end = loop {
        let read = file.read(&mut chunk).map_err(|e| io_kj_error(&e))?;
        let from = head.len();
        head.extend_from_slice(&chunk[..read]);
        if let Some(end) = blank_line(&head, from) {
            break Some(end);
        }
        if read == 0 || head.len() > MAX_HEAD {
            break None;
        }
    };
    let Some((end, (status, text, headers))) =
        end.and_then(|end| Some((end, parse_record(&head[..end], now)?)))
    else {
        drop(file);
        store.expire(key).await?;
        return Ok(None);
    };
    let start = end as u64 + 4;
    let size = file.metadata().map_err(|e| io_kj_error(&e))?.len();
    let size = size.saturating_sub(start);
    Ok(Some(Entry {
        file,
        status,
        text,
        headers,
        start,
        size,
    }))
}

/// Reads `body` to its end.
async fn drain(mut body: Pin<&mut AsyncInputStream>) -> Result<()> {
    let mut chunk = vec![0; 64 * 1024];
    while body.as_mut().try_read(&mut chunk, 1).await? > 0 {}
    Ok(())
}

/// One request to a [`CacheService`], for the entry of `key`, else of its URL, or a message
/// from cluster node `from`.
struct CacheRequest {
    cache: Rc<Cache>,
    key: Option<String>,
    from: Option<String>,
}

impl CacheRequest {
    /// Sends the response's head, for a body of `length` bytes.
    fn send<'a>(
        &self,
        response: ServiceResponse<'a>,
        status: u32,
        text: &str,
        headers: &HeaderMap,
        length: u64,
    ) -> Result<AsyncOutputStream<'a>> {
        let headers = kj_headers(header_table(&self.cache.factory), headers)?;
        response.send(status, text, HeadersRef::from(&*headers), Some(length))
    }

    /// Answers `status` with no headers and no body.
    fn answer(&self, response: ServiceResponse<'_>, status: u32, text: &str) -> Result<()> {
        self.send(response, status, text, &HeaderMap::new(), 0)
            .map(drop)
    }

    /// Answers `status` after reading `body` to its end.
    async fn refuse(
        &self,
        body: Pin<&mut AsyncInputStream>,
        response: ServiceResponse<'_>,
        status: u32,
        text: &str,
    ) -> Result<()> {
        drain(body).await?;
        self.answer(response, status, text)
    }

    async fn get(
        &self,
        key: &str,
        request: HeadersRef<'_>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let Some(mut entry) = load(&*self.cache.store, key, now_ms()).await? else {
            let mut miss = HeaderMap::new();
            miss.insert("cf-cache-status", HeaderValue::from_static("MISS"));
            return self
                .send(response, 504, "Gateway Timeout", &miss, 0)
                .map(drop);
        };
        let (headers, size) = (&mut entry.headers, entry.size);
        headers.insert("cf-cache-status", HeaderValue::from_static("HIT"));
        let asked = |name: &str| {
            request
                .get_by_name(name)
                .and_then(|v| std::str::from_utf8(v).ok())
        };
        let Some(ranges) = asked("Range").map_or(Some(Vec::new()), |r| parse_ranges(r, size))
        else {
            headers.remove(header::CONTENT_LENGTH);
            let range = header_value(format!("bytes */{size}"))?;
            headers.insert(header::CONTENT_RANGE, range);
            return self
                .send(response, 416, "Range Not Satisfiable", headers, 0)
                .map(drop);
        };
        if not_modified(asked("If-None-Match"), asked("If-Modified-Since"), headers) {
            headers.remove(header::CONTENT_LENGTH);
            return self
                .send(response, 304, "Not Modified", headers, 0)
                .map(drop);
        }
        match ranges[..] {
            [] => {
                let (status, text) = (u32::from(entry.status), entry.text.clone());
                self.body(response, entry, status, &text, 0, size).await
            }
            [(first, last)] => {
                let range = header_value(format!("bytes {first}-{last}/{size}"))?;
                headers.insert(header::CONTENT_RANGE, range);
                let length = last - first + 1;
                self.body(response, entry, 206, "Partial Content", first, length)
                    .await
            }
            _ => self.multipart(response, entry, &ranges).await,
        }
    }

    /// Sends `length` bytes of the entry's body from `first`.
    async fn body(
        &self,
        response: ServiceResponse<'_>,
        mut entry: Entry,
        status: u32,
        text: &str,
        first: u64,
        length: u64,
    ) -> Result<()> {
        let length_value = HeaderValue::from(length);
        entry.headers.insert(header::CONTENT_LENGTH, length_value);
        let mut out = self.send(response, status, text, &entry.headers, length)?;
        copy_file(&mut entry.file, entry.start + first, length, &mut out).await
    }

    /// Sends several ranges of the entry's body as `multipart/byteranges`.
    async fn multipart(
        &self,
        response: ServiceResponse<'_>,
        mut entry: Entry,
        ranges: &[(u64, u64)],
    ) -> Result<()> {
        let boundary = format!("workerd-boundary-{:032x}", rand::random::<u128>());
        let size = entry.size;
        let content_type = entry
            .headers
            .get(header::CONTENT_TYPE)
            .map(HeaderValue::as_bytes);
        let parts: Vec<_> = ranges
            .iter()
            .map(|&(first, last)| {
                let mut head = format!("--{boundary}\r\n").into_bytes();
                if let Some(t) = content_type {
                    head.extend([b"Content-Type: ".as_slice(), t, b"\r\n"].concat());
                }
                head.extend(format!("Content-Range: bytes {first}-{last}/{size}\r\n\r\n").bytes());
                (head, first, last - first + 1)
            })
            .collect();
        let close = format!("--{boundary}--");
        let length = parts
            .iter()
            .map(|(head, _, n)| head.len() as u64 + n + 2)
            .sum::<u64>()
            + close.len() as u64;
        let content_type = header_value(format!("multipart/byteranges; boundary={boundary}"))?;
        entry.headers.insert(header::CONTENT_TYPE, content_type);
        entry
            .headers
            .insert(header::CONTENT_LENGTH, HeaderValue::from(length));
        let mut out = self.send(response, 206, "Partial Content", &entry.headers, length)?;
        for (head, first, n) in parts {
            out.write(&head).await?;
            copy_file(&mut entry.file, entry.start + first, n, &mut out).await?;
            out.write(b"\r\n").await?;
        }
        out.write(close.as_bytes()).await
    }

    async fn put(
        &self,
        key: &str,
        request: HeadersRef<'_>,
        mut body: Pin<&mut AsyncInputStream>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let mut buf = Vec::new();
        let mut chunk = vec![0; 64 * 1024];
        let end = loop {
            let read = body.as_mut().try_read(&mut chunk, 1).await?;
            let from = buf.len();
            buf.extend_from_slice(&chunk[..read]);
            if let Some(end) = blank_line(&buf, from) {
                break Some(end);
            }
            if read == 0 || buf.len() > MAX_HEAD {
                break None;
            }
        };
        let head = end.and_then(|end| Some((parse_head(&buf[..end])?, &buf[end + 4..])));
        let Some(((status, text, mut headers), rest)) = head else {
            return self.refuse(body, response, 400, "Bad Request").await;
        };
        let now = now_ms();
        let authorization = request.get_by_name("Authorization").is_some();
        let ttl = lifetime(status, &mut headers, authorization, now);
        // An unreadable length matches no body.
        let length = headers
            .get(header::CONTENT_LENGTH)
            .map(|v| v.to_str().ok().and_then(|v| v.parse::<u64>().ok()));
        let Some(ttl) = ttl.filter(|_| length.flatten().is_none_or(|l| l <= MAX_ENTRY)) else {
            return self.refuse(body, response, 413, "Payload Too Large").await;
        };
        if ttl == 0 {
            drain(body).await?;
            let deleted = self.cache.store.delete(key, now).await?;
            return self.stored(response, deleted.map(|_| true));
        }
        // A head `load` could not read back is refused, as an unparsed one is.
        let record = record_head(now.saturating_add(ttl), status, &text, &headers);
        if record.len() > MAX_HEAD {
            return self.refuse(body, response, 400, "Bad Request").await;
        }
        let (temp, mut file) = self.cache.store.draft(key)?;
        let mut write = |bytes: &[u8]| file.write_all(bytes).map_err(|e| io_kj_error(&e));
        write(&record)?;
        write(rest)?;
        let mut size = rest.len() as u64;
        loop {
            if size > MAX_ENTRY {
                return self.refuse(body, response, 413, "Payload Too Large").await;
            }
            let read = body.as_mut().try_read(&mut chunk, 1).await?;
            if read == 0 {
                break;
            }
            write(&chunk[..read])?;
            size += read as u64;
        }
        if length.is_some_and(|l| l != Some(size)) {
            return self.answer(response, 400, "Bad Request");
        }
        let stored = self.cache.store.put(key, temp, file).await?;
        self.stored(response, stored)
    }

    /// Answers a put that was stored, or refused, or whose outcome is unknown: a 500, which
    /// `caches` ignores, as it does any status but 204 and 413, and the put resolves.
    fn stored(&self, response: ServiceResponse<'_>, stored: Option<bool>) -> Result<()> {
        match stored {
            Some(true) => self.answer(response, 204, "No Content"),
            Some(false) => self.answer(response, 413, "Payload Too Large"),
            None => self.answer(response, 500, "Internal Server Error"),
        }
    }

    /// Deletes a fresh entry, without reading it first: a read may miss where the entry lives.
    /// An outcome that is unknown is a 500, which makes `cache.delete()` reject.
    async fn purge(&self, key: &str, response: ServiceResponse<'_>) -> Result<()> {
        match self.cache.store.delete(key, now_ms()).await? {
            Some(true) => self.answer(response, 200, "OK"),
            Some(false) => self.answer(response, 404, "Not Found"),
            None => self.answer(response, 500, "Internal Server Error"),
        }
    }

    async fn serve<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        if let Some(from) = &self.from {
            let store = &self.cache.store;
            return store
                .message(from, method, url, headers, body, response)
                .await;
        }
        let namespace = headers
            .get_by_name("CF-Cache-Namespace")
            .unwrap_or_default();
        let key = entry_name(namespace, self.key.as_ref().map_or(url, String::as_bytes));
        match method {
            Method::GET => self.get(&key, headers, response).await,
            Method::PUT => self.put(&key, headers, body, response).await,
            Method::PURGE => self.purge(&key, response).await,
            _ => self.refuse(body, response, 405, "Method Not Allowed").await,
        }
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
    ) -> ServiceResult {
        Ok(self.serve(method, url, headers, body, response).await?)
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> ServiceResult {
        Err(unsupported("Cache services").into())
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for CacheRequest {
    async fn run_scheduled(
        &mut self,
        _time: &SystemTime,
        _cron: &str,
    ) -> worker::Result<ScheduledResult> {
        Err(unsupported("Cache services").into())
    }

    async fn run_alarm(
        &mut self,
        _time: &SystemTime,
        _retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        Err(unsupported("Cache services").into())
    }
}

#[path = "cache-cluster.rs"]
mod clustered;

#[cfg(test)]
#[path = "cache-test.rs"]
mod tests;
