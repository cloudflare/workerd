// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `bucket` service (`ClusterBucket` in workerd.capnp): objects served
//! over a small S3-like HTTP API, the one `r2.js` maps R2's onto.
//!
//! The objects are a directory of the cluster's bucket, or without a cluster
//! a local store (`local.rs`).
//!
//! A key is the request's path less its leading `/`, percent-decoded whole,
//! so `%2Fa%2F%2Fb` is `/a//b`. Requests:
//!
//! - `HEAD`, `GET /<key>` (with `Range`), `PUT /<key>`, `DELETE /<key>`.
//! - `GET /?list&prefix=&start-after=&delimiter=&max=&token=`.
//! - `POST /<key>?uploads`; `PUT /<key>?part=<n>&upload=<id>`; `GET`,
//!   `POST` and `DELETE /<key>?upload=<id>`: list, complete, abort.
//!
//! An object's answer carries `ETag`, `x-version` (what `If-Match` names),
//! `Last-Modified`, `x-modified` (ms), the five content headers and user
//! metadata as `x-meta-<name>`. A refusal is JSON, `{code, message}`, with
//! S3's code.

mod local;

use std::path::Path;
use std::pin::Pin;
use std::rc::Rc;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use bytes::Bytes;
use cluster::Body;
use cluster::Bucket;
use cluster::Guard;
use cluster::Headers as Content;
use cluster::Object;
use cluster::Payload;
use cluster::Range;
use cluster::Version;
use cluster::Written;
use futures::StreamExt;
use httpdate::fmt_http_date;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service as HttpService;
use kj::http::ServiceResponse;
use kj::http::ServiceResult;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use percent_encoding::percent_decode_str;
use serde_json::json;
use worker::AlarmResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::cluster_bucket;

use crate::Result;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::text;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::disk::path_segments;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::unsupported;

/// A PUT's body is read and stored a part at a time: one write up to this,
/// a multipart upload of parts this large beyond, so a request holds about
/// one part in memory.
const PART: usize = 8 << 20;

/// The largest part `PUT ?part` takes, which it holds in memory; R2 takes
/// up to 5 GiB.
const MAX_PART: usize = 64 << 20;

/// The largest completion's body, its `[{part, etag}]`.
const MAX_COMPLETE: usize = 1 << 20;

/// The most keys and prefixes one page of a listing has, as S3's.
const MAX_LIST: usize = 1000;

struct Shared {
    bucket: Bucket,
    factory: Rc<Factory>,
}

pub struct BucketService(Rc<Shared>);

/// Where a `bucket` service's objects are.
#[derive(Clone, Copy)]
pub enum Home<'a> {
    Cluster(&'a ClusterNode),
    /// A local store under this `localDisk` directory.
    Disk(&'a Path),
}

pub fn make_bucket_service(
    conf: cluster_bucket::Reader<'_>,
    home: Home<'_>,
    factory: &Rc<Factory>,
) -> Result<BucketService> {
    let path = text(conf.get_path())?;
    let url = format!("http://bucket/{path}");
    if path.is_empty() || path_segments(&url).is_none_or(|s| s.join("/") != path) {
        return Err(kj::failed!(
            "bucket path {path:?} must be `/`-separated names."
        ));
    }
    let bucket = match home {
        Home::Cluster(node) => node.cluster.services(&path),
        Home::Disk(disk) => {
            let root = disk.join(&path);
            let store = local::Local::open(&root, conf.get_min_part_size());
            let store = store.map_err(|e| kj::failed!("{}: {e}", root.display()))?;
            Bucket::new(store, "")
        }
    };
    Ok(BucketService(Rc::new(Shared {
        bucket,
        factory: Rc::clone(factory),
    })))
}

impl Channel for BucketService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(BucketRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Bucket services"))
    }
}

struct BucketRequest(Rc<Shared>);

/// Why a request failed: an answer to send, or the request's own error
/// (its body broke off), which fails it.
enum Fail {
    Refuse {
        status: u16,
        code: String,
        message: String,
    },
    Kj(crate::Error),
}

fn refuse(status: u16, code: &str, message: impl Into<String>) -> Fail {
    Fail::Refuse {
        status,
        code: code.to_owned(),
        message: message.into(),
    }
}

impl From<cluster::Error> for Fail {
    fn from(error: cluster::Error) -> Self {
        match error {
            cluster::Error::Refused { what, status, code } => Self::Refuse {
                // A refusal in a 200, as S3 answers a completion it failed.
                status: if status < 400 { 500 } else { status },
                code,
                message: what,
            },
            // A guarded write that may have landed.
            e @ cluster::Error::OutcomeUnknown(_) => refuse(500, "OutcomeUnknown", e.to_string()),
            e => refuse(500, "InternalError", e.to_string()),
        }
    }
}

impl From<crate::Error> for Fail {
    fn from(error: crate::Error) -> Self {
        Self::Kj(error)
    }
}

type Answer<T> = std::result::Result<T, Fail>;

/// What a request answers.
enum Reply {
    Empty(u16, http::HeaderMap),
    /// A HEAD's answer: the length of the body a GET would have.
    Head(http::HeaderMap, u64),
    Json(u16, serde_json::Value),
    /// A body of this length.
    Stream(u16, http::HeaderMap, u64, Body),
}

const fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        204 => "No Content",
        206 => "Partial Content",
        400 => "Bad Request",
        404 => "Not Found",
        412 => "Precondition Failed",
        416 => "Range Not Satisfiable",
        501 => "Not Implemented",
        _ => "Error",
    }
}

/// The request's headers, as `http` has them: names in lower case.
fn header_map(headers: HeadersRef<'_>) -> http::HeaderMap {
    let mut map = http::HeaderMap::new();
    kj_hyper::Head::new(headers.as_ffi()).apply(&mut map, &mut http::Extensions::new());
    map
}

fn not_ascii(name: &str) -> Fail {
    refuse(
        400,
        "InvalidArgument",
        format!("{name}: not an ASCII value"),
    )
}

/// A request header's value, which must be ASCII.
fn ascii(headers: &http::HeaderMap, name: &str) -> Answer<Option<String>> {
    let Some(value) = headers.get(name) else {
        return Ok(None);
    };
    let value = value.to_str().map_err(|_| not_ascii(name))?;
    Ok(Some(value.to_owned()))
}

/// A write's content headers and `x-meta-*` user metadata.
fn described(headers: &http::HeaderMap) -> Answer<(Content, Vec<(String, String)>)> {
    for (name, _) in Content::default().named() {
        ascii(headers, name)?;
    }
    let content = Content::from_fn(|name| ascii(headers, name).ok().flatten());
    let mut metadata = Vec::new();
    for name in headers.keys() {
        let Some(meta) = name.as_str().strip_prefix("x-meta-") else {
            continue;
        };
        for value in headers.get_all(name) {
            let value = value.to_str().map_err(|_| not_ascii(name.as_str()))?;
            metadata.push((meta.to_owned(), value.to_owned()));
        }
    }
    Ok((content, metadata))
}

/// A write's guard: `If-Match: <x-version>` or `If-None-Match: *`.
fn guard(headers: &http::HeaderMap) -> Answer<Option<Guard>> {
    match (
        ascii(headers, "if-match")?,
        ascii(headers, "if-none-match")?,
    ) {
        (None, None) => Ok(None),
        (Some(version), None) => Ok(Some(Guard::Match(Version::new(version)))),
        (None, Some(star)) if star == "*" => Ok(Some(Guard::Absent)),
        _ => Err(refuse(
            400,
            "InvalidArgument",
            "a write takes If-Match: <x-version> or If-None-Match: *",
        )),
    }
}

/// `bytes=a-b`, `bytes=a-` or `bytes=-n`; anything else is refused, not
/// ignored as S3 does, so that a caller's mistake shows.
fn range(header: &str) -> Answer<Range> {
    let invalid = || refuse(400, "InvalidArgument", format!("Range: {header}"));
    let spec = header.strip_prefix("bytes=").ok_or_else(invalid)?;
    let (first, last) = spec.split_once('-').ok_or_else(invalid)?;
    let number = |text: &str| text.trim().parse::<u64>().map_err(|_| invalid());
    match (first.trim(), last.trim()) {
        ("", last) => Ok(Range::Suffix(number(last)?)),
        (first, "") => Ok(Range::From {
            offset: number(first)?,
            length: None,
        }),
        (first, last) => {
            let (first, last) = (number(first)?, number(last)?);
            // A last byte past any size means to the end, as for any other.
            let length = last.saturating_add(1).checked_sub(first).filter(|&n| n > 0);
            Ok(Range::From {
                offset: first,
                length: Some(length.ok_or_else(invalid)?),
            })
        }
    }
}

fn append(map: &mut http::HeaderMap, name: &str, value: impl AsRef<str>) -> Answer<()> {
    let name = http::HeaderName::from_bytes(name.as_bytes())
        .map_err(|_| refuse(500, "InternalError", format!("header name {name:?}")))?;
    map.append(name, header_value(value)?);
    Ok(())
}

/// The headers that name a version of an object.
fn versioned(etag: &str, version: &Version, modified_ms: u64) -> Answer<http::HeaderMap> {
    let mut map = http::HeaderMap::new();
    let modified = fmt_http_date(UNIX_EPOCH + Duration::from_millis(modified_ms));
    append(&mut map, "etag", format!("\"{etag}\""))?;
    append(&mut map, "x-version", version.as_str())?;
    append(&mut map, "last-modified", modified)?;
    append(&mut map, "x-modified", modified_ms.to_string())?;
    Ok(map)
}

/// A HEAD's or a GET's headers for `object`.
fn object_headers(object: &Object) -> Answer<http::HeaderMap> {
    let mut map = versioned(&object.etag, &object.version, object.modified_ms)?;
    for (name, value) in object.headers.named() {
        if let Some(value) = value {
            append(&mut map, name, value)?;
        }
    }
    for (name, value) in &object.metadata {
        append(&mut map, &format!("x-meta-{name}"), value)?;
    }
    if let Some((offset, length)) = object.range {
        let last = (offset + length).saturating_sub(1);
        let range = format!("bytes {offset}-{last}/{}", object.size);
        append(&mut map, "content-range", range)?;
    }
    Ok(map)
}

/// The path and query of an absolute URL, or of a request target, which
/// may hold `://` in a key. Not `url::Url`, which would resolve dot segments
/// in a key.
fn target(url: &str) -> &str {
    match url.split_once("://") {
        Some((_, rest)) if !url.starts_with('/') => rest.find('/').map_or("", |i| &rest[i..]),
        _ => url,
    }
}

/// Fills `buffer` from `body` up to `limit` bytes: whether it is full. It
/// doubles as it is read, so a small body costs a small buffer and each
/// byte is zeroed about once.
async fn fill(
    body: &mut Pin<&mut AsyncInputStream>,
    buffer: &mut Vec<u8>,
    limit: usize,
) -> Result<bool> {
    let mut filled = buffer.len();
    let full = loop {
        if filled >= limit {
            break true;
        }
        if filled == buffer.len() {
            buffer.resize(limit.min(filled.max(64 << 10) * 2), 0);
        }
        let read = body.as_mut().try_read(&mut buffer[filled..], 1).await?;
        if read == 0 {
            break false;
        }
        filled += read;
    };
    buffer.truncate(filled);
    Ok(full)
}

/// Reads the rest of a part into `part`, and when it fills, one byte past
/// it into `next`, which stays empty at the end of the body.
async fn next_part(
    body: &mut Pin<&mut AsyncInputStream>,
    part: &mut Vec<u8>,
    next: &mut Vec<u8>,
) -> Result<()> {
    if fill(body, part, PART).await? {
        fill(body, next, 1).await?;
    }
    Ok(())
}

/// The value of the query parameter `name`.
fn param<'a>(query: &'a [(String, String)], name: &str) -> Option<&'a str> {
    let found = query.iter().find(|(n, _)| n == name);
    found.map(|(_, value)| value.as_str())
}

impl BucketRequest {
    async fn handle(
        &self,
        method: Method,
        url: &[u8],
        headers: &http::HeaderMap,
        mut body: Pin<&mut AsyncInputStream>,
    ) -> Answer<Reply> {
        let url = std::str::from_utf8(url)
            .map_err(|_| refuse(400, "InvalidArgument", "the URL is not UTF-8"))?;
        let path = target(url);
        let (path, query) = path.split_once('?').unwrap_or((path, ""));
        let key = percent_decode_str(path.strip_prefix('/').unwrap_or(path))
            .decode_utf8()
            .map_err(|_| refuse(400, "InvalidArgument", "the key is not UTF-8"))?;
        // A binding reaches only its own directory, even through a store or
        // proxy that resolves dot segments. Empty segments stay: R2 keys may
        // be `a//b`, and collapsing them cannot leave the directory.
        if key.split('/').any(|s| s == "." || s == "..") {
            return Err(refuse(
                400,
                "InvalidArgument",
                "a key has a . or .. segment",
            ));
        }
        let query: Vec<(String, String)> = url::form_urlencoded::parse(query.as_bytes())
            .into_owned()
            .collect();
        let param = |name| param(&query, name);
        let bucket = &self.0.bucket;
        if key.is_empty() {
            return match (method, param("list")) {
                (Method::GET, Some(_)) => self.list(&query).await,
                _ => Err(refuse(404, "NoSuchKey", "no key")),
            };
        }
        let key = &key[..];
        match (method, param("uploads"), param("upload")) {
            (Method::POST, Some(_), None) => {
                let (content, metadata) = described(headers)?;
                let id = bucket.begin(key, &content, &metadata).await?;
                Ok(Reply::Json(200, json!({ "uploadId": id })))
            }
            (Method::PUT, None, Some(upload)) => {
                let n = param("part").and_then(|n| n.parse().ok());
                let n = n.ok_or_else(|| refuse(400, "InvalidArgument", "part=<n>"))?;
                let (mut part, mut more) = (Vec::new(), Vec::new());
                if fill(&mut body, &mut part, MAX_PART).await?
                    && fill(&mut body, &mut more, 1).await?
                {
                    let message = format!("a part over {MAX_PART} bytes");
                    return Err(refuse(400, "EntityTooLarge", message));
                }
                let etag = bucket.part(key, upload, n, part.into()).await?;
                Ok(Reply::Json(200, json!({ "etag": etag })))
            }
            (Method::POST, None, Some(upload)) => {
                let mut sent = Vec::new();
                if fill(&mut body, &mut sent, MAX_COMPLETE).await? {
                    return Err(refuse(400, "MalformedXML", "too many parts"));
                }
                let malformed = || refuse(400, "MalformedXML", "expected [{part, etag}]");
                let parts: Vec<serde_json::Value> =
                    serde_json::from_slice(&sent).map_err(|_| malformed())?;
                let parts = parts.iter().map(|p| {
                    let n = p["part"].as_u64().and_then(|n| u32::try_from(n).ok());
                    Some((n?, p["etag"].as_str()?.to_owned()))
                });
                let parts = parts.collect::<Option<Vec<_>>>().ok_or_else(malformed)?;
                let written = bucket.complete(key, upload, &parts).await?;
                self.written(key, written).await
            }
            (Method::GET, None, Some(upload)) => {
                let parts = bucket.parts(key, upload).await?;
                let parts = parts
                    .iter()
                    .map(|(n, etag, size)| json!({ "part": n, "etag": etag, "size": size }));
                Ok(Reply::Json(200, parts.collect()))
            }
            (Method::DELETE, None, Some(upload)) => {
                bucket.abort(key, upload).await?;
                Ok(Reply::Empty(204, http::HeaderMap::new()))
            }
            (_, None, None) => self.object(method, key, headers, body).await,
            _ => Err(refuse(400, "InvalidArgument", "unknown request")),
        }
    }

    async fn object(
        &self,
        method: Method,
        key: &str,
        headers: &http::HeaderMap,
        mut body: Pin<&mut AsyncInputStream>,
    ) -> Answer<Reply> {
        let bucket = &self.0.bucket;
        let absent = || refuse(404, "NoSuchKey", key);
        match method {
            Method::HEAD => {
                let object = bucket.head(key).await?.ok_or_else(absent)?;
                Ok(Reply::Head(object_headers(&object)?, object.size))
            }
            Method::GET => {
                let range = ascii(headers, "range")?.map(|r| range(&r)).transpose()?;
                let (object, body) = bucket.read(key, range).await?.ok_or_else(absent)?;
                let length = object.range.map_or(object.size, |(_, length)| length);
                let status = if object.range.is_some() { 206 } else { 200 };
                let headers = object_headers(&object)?;
                Ok(Reply::Stream(status, headers, length, body))
            }
            Method::PUT => {
                let guard = guard(headers)?;
                let (content, metadata) = described(headers)?;
                let (mut part, mut next) = (Vec::new(), Vec::new());
                next_part(&mut body, &mut part, &mut next).await?;
                if next.is_empty() {
                    let body = Payload::Bytes(part.into());
                    let written = bucket.write(key, body, guard, &content, &metadata).await?;
                    let written = written.ok_or_else(|| refuse(412, "PreconditionFailed", key))?;
                    return self.written(key, written).await;
                }
                // A completion takes no precondition.
                if guard.is_some() {
                    let message = format!("a conditional write of over {PART} bytes");
                    return Err(refuse(400, "InvalidArgument", message));
                }
                let upload = bucket.begin(key, &content, &metadata).await?;
                let written = self.upload(key, &upload, part, next, body).await;
                if written.is_err() {
                    let _ = bucket.abort(key, &upload).await;
                }
                self.written(key, written?).await
            }
            Method::DELETE => {
                bucket.delete(key).await?;
                Ok(Reply::Empty(204, http::HeaderMap::new()))
            }
            _ => Err(refuse(501, "NotImplemented", "method")),
        }
    }

    /// Uploads `part`, whose next byte is `next`, then the rest of `body`,
    /// as the parts of `upload`.
    async fn upload(
        &self,
        key: &str,
        upload: &str,
        mut part: Vec<u8>,
        mut next: Vec<u8>,
        mut body: Pin<&mut AsyncInputStream>,
    ) -> Answer<Written> {
        let bucket = &self.0.bucket;
        let mut parts = Vec::new();
        for n in 1.. {
            let etag = bucket.part(key, upload, n, Bytes::from(part)).await?;
            parts.push((n, etag));
            if next.is_empty() {
                break;
            }
            part = std::mem::take(&mut next);
            next_part(&mut body, &mut part, &mut next).await?;
        }
        Ok(bucket.complete(key, upload, &parts).await?)
    }

    /// A write's answer. A store that does not answer a completion's
    /// version (Cloud Storage) is asked for it.
    async fn written(&self, key: &str, written: Written) -> Answer<Reply> {
        let (version, modified) = if let Some(version) = written.version {
            (version, written.modified_ms.unwrap_or_else(cluster::now_ms))
        } else {
            let object = self.0.bucket.head(key).await?;
            let object = object.ok_or_else(|| refuse(500, "InternalError", key))?;
            (object.version, object.modified_ms)
        };
        let headers = versioned(&written.etag, &version, modified)?;
        Ok(Reply::Empty(200, headers))
    }

    async fn list(&self, query: &[(String, String)]) -> Answer<Reply> {
        let some = |name| param(query, name).filter(|v| !v.is_empty());
        let max = match some("max") {
            None => MAX_LIST,
            Some(max) => max
                .parse::<usize>()
                .ok()
                .filter(|&n| n > 0)
                .ok_or_else(|| refuse(400, "InvalidArgument", format!("max={max}")))?
                .min(MAX_LIST),
        };
        let prefix = some("prefix").unwrap_or("");
        let (after, delimiter, token) = (some("start-after"), some("delimiter"), some("token"));
        let page = self.0.bucket.page(prefix, after, delimiter, max, token);
        let page = page.await?;
        let objects = page.entries.iter().map(|e| {
            json!({
                "key": e.key,
                "size": e.size,
                "etag": e.etag,
                "version": e.version.as_str(),
                "modified": e.modified_ms,
            })
        });
        let objects: Vec<_> = objects.collect();
        let listed = json!({ "objects": objects, "prefixes": page.prefixes, "token": page.token });
        Ok(Reply::Json(200, listed))
    }

    async fn send(&self, reply: Reply, response: ServiceResponse<'_>) -> Result<()> {
        let table = header_table(&self.0.factory);
        let (status, mut map, length, body) = match reply {
            Reply::Empty(status, map) => (status, map, 0, None),
            Reply::Head(map, length) => (200, map, length, None),
            Reply::Json(status, value) => {
                let mut map = http::HeaderMap::new();
                map.insert(
                    http::header::CONTENT_TYPE,
                    header_value("application/json")?,
                );
                let json = Bytes::from(value.to_string());
                let length = json.len() as u64;
                let body = futures::stream::iter([Ok(json)]).boxed_local();
                (status, map, length, Some(body))
            }
            Reply::Stream(status, map, length, body) => (status, map, length, Some(body)),
        };
        // Set explicitly, as the `disk` service does, so that a worker
        // calling in-process sees it too. A 204 has none.
        let length = (status != 204).then_some(length);
        if let Some(length) = length {
            map.insert(
                http::header::CONTENT_LENGTH,
                header_value(length.to_string())?,
            );
        }
        let headers = kj_headers(table, &map)?;
        let headers = HeadersRef::from(&*headers);
        let mut out = response.send(status.into(), reason(status), headers, length)?;
        let Some(mut body) = body else {
            return Ok(());
        };
        while let Some(chunk) = body.next().await {
            let chunk = chunk.map_err(|e| kj::disconnected!("{e}"))?;
            out.write(&chunk).await?;
        }
        Ok(())
    }

    /// Answers a request on the bucket.
    async fn respond<'a>(
        &'a self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let headers = header_map(headers);
        let reply = match self.handle(method, url, &headers, body).await {
            Ok(reply) => reply,
            Err(Fail::Kj(error)) => return Err(error),
            Err(Fail::Refuse {
                status,
                code,
                message,
            }) => Reply::Json(status, json!({ "code": code, "message": message })),
        };
        self.send(reply, response).await
    }
}

#[async_trait::async_trait(?Send)]
impl HttpService for BucketRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> ServiceResult {
        Ok(self.respond(method, url, headers, body, response).await?)
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> ServiceResult {
        Err(unsupported("Bucket services").into())
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for BucketRequest {
    async fn run_scheduled(
        &mut self,
        _time: &SystemTime,
        _cron: &str,
    ) -> worker::Result<ScheduledResult> {
        Err(unsupported("Bucket services").into())
    }

    async fn run_alarm(
        &mut self,
        _time: &SystemTime,
        _retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        Err(unsupported("Bucket services").into())
    }
}

#[cfg(test)]
#[path = "bucket-test.rs"]
mod tests;
