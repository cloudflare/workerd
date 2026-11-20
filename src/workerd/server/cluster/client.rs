// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A bucket's HTTP client, the calls the cluster makes (`Store`), in the
//! dialect of the bucket's API (`Dialect`): S3's (`s3.rs`) or Cloud
//! Storage's XML API (`gcs.rs`). Requests use path-style URLs,
//! `/<bucket>/<key>`. The dialect authorizes each request and names the
//! headers of versions, preconditions and metadata; the client sends the
//! requests, retries those the bucket refused under load, bounds how long
//! each takes, reads `ListObjectsV2` listings and measures this node's
//! clock against the bucket's.

use std::cell::Cell;
use std::fmt::Write;
use std::path::Path;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use bytes::Bytes;
use futures::StreamExt;

use crate::Error;
use crate::Result;
use crate::bucket::Body;
use crate::bucket::Entry;
use crate::bucket::Guard;
use crate::bucket::Headers;
use crate::bucket::Object;
use crate::bucket::Page;
use crate::bucket::Range;
use crate::bucket::Store;
use crate::bucket::Version;
use crate::bucket::Written;
use crate::http::Call;
use crate::http::Failure;
use crate::http::Http;
use crate::http::HttpError;
use crate::http::HttpResponse;
use crate::http::Payload;

/// What a bucket API's dialect decides about a request.
pub trait Dialect {
    /// The prefix of the headers that carry an object's user metadata.
    const METADATA: &'static str;

    /// The name of the header that carries an object's version.
    const VERSION: &'static str;

    /// Headers every read of an object (`Store::head`, `Store::read`)
    /// carries.
    const READ: &'static [(&'static str, &'static str)] = &[];

    /// The version of the object a successful read or write answers, empty
    /// when the answer has none.
    fn version(response: &HttpResponse) -> &str;

    /// The header that makes a write conditional on `guard`; an error for a
    /// version this store cannot have answered.
    fn precondition(guard: &Guard) -> std::result::Result<(String, String), String>;

    /// Adds the headers that authorize `request`; answers the credential
    /// they carry, if `refused` is to hear of it.
    fn authorize(&self, request: &mut Request<'_>) -> impl Future<Output = Result<Option<String>>>;

    /// Drops `credential`, which the bucket refused (401). Answers whether
    /// to send the request again, with another.
    fn refused(&self, _credential: &str) -> bool {
        false
    }
}

/// A request, as its dialect authorizes it.
pub struct Request<'a> {
    pub method: &'a str,
    /// The endpoint's `host[:port]`.
    pub host: &'a str,
    /// `/<bucket>/<key>`, URI-encoded.
    pub path: &'a str,
    /// The query's `name=value` pairs, URI-encoded, sorted and joined with
    /// `&`.
    pub query: &'a str,
    /// The headers so far, to which the dialect adds its own.
    pub headers: Vec<(String, String)>,
}

/// The client of the bucket `bucket` at `endpoint`, in dialect `D`.
pub struct Client<D> {
    dialect: D,
    http: Http,
    /// `<scheme>://<host>[:<port>]`.
    endpoint: String,
    bucket: String,
    /// This node's wall clock less the bucket's, at the last response.
    skew_ms: Cell<Option<i64>>,
    /// The first pause of the 429/503 backoff (`BACKOFF`).
    backoff: Duration,
}

impl<D> Client<D> {
    pub fn with_dialect(dialect: D, http: Http, endpoint: &str, bucket: &str) -> Self {
        Self {
            dialect,
            http,
            endpoint: endpoint.trim_end_matches('/').to_owned(),
            bucket: bucket.to_owned(),
            skew_ms: Cell::new(None),
            backoff: BACKOFF,
        }
    }
}

#[cfg(test)]
impl<D> Client<D> {
    pub const fn dialect(&self) -> &D {
        &self.dialect
    }

    pub fn endpoint(&self) -> &str {
        &self.endpoint
    }

    /// With the 429/503 backoff's first pause `pause` rather than
    /// `BACKOFF`, the others in proportion: for tests that would otherwise
    /// wait out seconds of it.
    #[must_use]
    pub const fn backoff(mut self, pause: Duration) -> Self {
        self.backoff = pause;
        self
    }
}

#[async_trait::async_trait(?Send)]
impl<D: Dialect> Store for Client<D> {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        let empty = none();
        let response = self
            .send(&call_for("GET", false, &empty, to), key, &[], Vec::new())
            .await?;
        match response.status {
            200 => {
                let version = version::<D>("GET", key, &response)?;
                Ok(Some((response.body, version)))
            }
            404 => Ok(None),
            _ => Err(failure("GET", key, &response)),
        }
    }

    /// `Ok(None)` when the guard failed and nothing was written.
    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        let headers = metadata
            .iter()
            .map(|(name, value)| (format!("{}{name}", D::METADATA), value.clone()))
            .collect();
        let Some(sent) = self.put_if(key, body, guard.as_ref(), headers).await? else {
            return Ok(None);
        };
        Ok(Some(version::<D>("PUT", key, &sent.response)?))
    }

    async fn delete(&self, key: &str) -> Result<()> {
        let empty = none();
        let delete = call_for("DELETE", false, &empty, None);
        let response = self.send(&delete, key, &[], Vec::new()).await?;
        match response.status {
            200 | 204 | 404 => Ok(()),
            _ => Err(failure("DELETE", key, &response)),
        }
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        Ok(self.listing(prefix, false).await?.0)
    }

    async fn prefixes(&self, prefix: &str) -> Result<Vec<String>> {
        Ok(self.listing(prefix, true).await?.1)
    }

    /// How far this node's wall clock is outside the bucket's, from the last
    /// response's `Date` header: 0 when the header fits the request's window.
    fn clock_skew_ms(&self) -> Option<i64> {
        self.skew_ms.get()
    }

    // R2's calls. A read is a `Repeat::Read`: `http::prompt` may send it
    // twice, and only the wait for its answer's head, so a read streams the
    // body of the copy that answered first. A write goes as `put` does:
    // twice at once only if guarded, with a small body. The multipart calls
    // are never sent twice at once, being neither GETs nor guarded PUTs, nor
    // `Repeat::Delete`s but for `abort`; each is sent again after a refusal
    // under load or a failed connection (`exchange`), which is safe: a part
    // uploaded again replaces itself, an upload begun again leaves the first
    // one unused, and a completion or an abort sent again after one that
    // landed finds the upload gone (404 `NoSuchUpload`).

    async fn head(&self, key: &str) -> Result<Option<Object>> {
        let empty = none();
        let head = call_for("HEAD", false, &empty, None);
        let response = self.send(&head, key, &[], reading::<D>()).await?;
        match response.status {
            200 => Ok(Some(object::<D>("HEAD", key, &response)?)),
            404 => Ok(None),
            _ => Err(refused("HEAD", key, &response)),
        }
    }

    async fn read(&self, key: &str, range: Option<Range>) -> Result<Option<(Object, Body)>> {
        let mut headers = reading::<D>();
        headers.extend(range.map(|range| ("range".to_owned(), range.header())));
        let empty = none();
        let get = Call {
            stream: true,
            ..call_for("GET", false, &empty, None)
        };
        let mut response = self.send(&get, key, &[], headers).await?;
        match response.status {
            200 | 206 => {}
            404 => return Ok(None),
            _ => return Err(refused("GET", key, &response)),
        }
        let object = object::<D>("GET", key, &response)?;
        let what = format!("GET {key}");
        let body = futures::stream::iter(response.stream.take()).flatten();
        let body = body.map(move |chunk| chunk.map_err(|e| Error::Http(format!("{what}: {e}"))));
        Ok(Some((object, body.boxed_local())))
    }

    async fn page(
        &self,
        prefix: &str,
        start_after: Option<&str>,
        delimiter: Option<&str>,
        max: usize,
        token: Option<&str>,
    ) -> Result<Page> {
        let mut query = vec![
            ("list-type", "2".to_owned()),
            ("prefix", prefix.to_owned()),
            ("max-keys", max.to_string()),
        ];
        query.extend(start_after.map(|key| ("start-after", key.to_owned())));
        query.extend(delimiter.map(|delimiter| ("delimiter", delimiter.to_owned())));
        query.extend(token.map(|token| ("continuation-token", token.to_owned())));
        let empty = none();
        let list = call_for("GET", false, &empty, None);
        let response = self.send(&list, "", &query, Vec::new()).await?;
        if response.status != 200 {
            return Err(refused("LIST", prefix, &response));
        }
        let mut page = Page::default();
        page.token = listing_page(
            prefix,
            &response.body,
            &mut page.entries,
            &mut page.prefixes,
        )?;
        Ok(page)
    }

    async fn write(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        headers: &Headers,
        metadata: &[(String, String)],
    ) -> Result<Option<Written>> {
        let headers = described::<D>(headers, metadata);
        let Some(sent) = self.put_if(key, body, guard.as_ref(), headers).await? else {
            return Ok(None);
        };
        let version = version::<D>("PUT", key, &sent.response)?;
        Ok(Some(Written {
            version: Some(version),
            etag: unquote(&sent.response.etag),
            modified_ms: None,
        }))
    }

    async fn begin(
        &self,
        key: &str,
        headers: &Headers,
        metadata: &[(String, String)],
    ) -> Result<String> {
        let empty = none();
        let begin = call_for("POST", false, &empty, None);
        let query = [("uploads", String::new())];
        let headers = described::<D>(headers, metadata);
        let response = self.send(&begin, key, &query, headers).await?;
        if response.status != 200 {
            return Err(refused("BEGIN", key, &response));
        }
        let xml = String::from_utf8_lossy(&response.body);
        let id = elements(&xml, "UploadId").next().map(unescape);
        id.filter(|id| !id.is_empty())
            .ok_or_else(|| Error::Http(format!("BEGIN {key}: 200 without an UploadId")))
    }

    async fn part(&self, key: &str, upload: &str, n: u32, body: Bytes) -> Result<String> {
        let body = Payload::Bytes(body);
        let part = call_for("PUT", false, &body, None);
        let query = [
            ("partNumber", n.to_string()),
            ("uploadId", upload.to_owned()),
        ];
        let response = self.send(&part, key, &query, Vec::new()).await?;
        match (response.status, unquote(&response.etag)) {
            (200, etag) if !etag.is_empty() => Ok(etag),
            (200, _) => Err(Error::Http(format!("PART {key}: 200 without an ETag"))),
            _ => Err(refused("PART", key, &response)),
        }
    }

    async fn complete(&self, key: &str, upload: &str, parts: &[(u32, String)]) -> Result<Written> {
        let mut xml = "<CompleteMultipartUpload>".to_owned();
        for (n, etag) in parts {
            let etag = etag.trim_matches('"');
            let _ = write!(
                xml,
                "<Part><PartNumber>{n}</PartNumber><ETag>\"{etag}\"</ETag></Part>"
            );
        }
        xml += "</CompleteMultipartUpload>";
        let body = Payload::Bytes(xml.into());
        let complete = call_for("POST", false, &body, None);
        let query = [("uploadId", upload.to_owned())];
        let sent = self.exchange(&complete, key, &query, Vec::new()).await?;
        let mut response = sent.response;
        let xml = String::from_utf8_lossy(&response.body).into_owned();
        // S3 sends its 200 before it assembles the object: a failure to
        // assemble it comes as an error in the body.
        if response.status != 200 || xml.contains("<Error>") {
            if response.status == 404 && sent.maybe_applied {
                return Err(Error::OutcomeUnknown(format!(
                    "COMPLETE {key}: 404 to a retry"
                )));
            }
            return Err(refused("COMPLETE", key, &response));
        }
        // S3 answers the `ETag` in the body alone; it is its version too.
        let etag = elements(&xml, "ETag").next().map(unescape);
        if let Some(etag) = etag.filter(|_| response.etag.is_empty()) {
            response.etag = etag;
        }
        let version = Some(D::version(&response)).filter(|v| !v.is_empty());
        Ok(Written {
            version: version.map(|v| Version(v.to_owned())),
            etag: unquote(&response.etag),
            modified_ms: None,
        })
    }

    async fn abort(&self, key: &str, upload: &str) -> Result<()> {
        let empty = none();
        let abort = call_for("DELETE", false, &empty, None);
        let query = [("uploadId", upload.to_owned())];
        let sent = self.exchange(&abort, key, &query, Vec::new()).await?;
        match sent.response.status {
            200 | 204 => Ok(()),
            404 if sent.maybe_applied => Ok(()),
            _ => Err(refused("ABORT", key, &sent.response)),
        }
    }

    /// `ListParts`, page by page.
    async fn parts(&self, key: &str, upload: &str) -> Result<Vec<(u32, String, u64)>> {
        let mut parts = Vec::new();
        let mut marker = None;
        loop {
            let mut query = vec![("uploadId", upload.to_owned())];
            query.extend(marker.take().map(|m| ("part-number-marker", m)));
            let empty = none();
            let list = call_for("GET", false, &empty, None);
            let response = self.send(&list, key, &query, Vec::new()).await?;
            if response.status != 200 {
                return Err(refused("PARTS", key, &response));
            }
            let xml = String::from_utf8_lossy(&response.body);
            for part in elements(&xml, "Part") {
                let text = |name| elements(part, name).next().map(unescape);
                let n = text("PartNumber").and_then(|n| n.parse().ok());
                let size = text("Size").and_then(|n| n.parse().ok()).unwrap_or(0);
                let etag = unquote(&text("ETag").unwrap_or_default());
                parts.extend(n.map(|n| (n, etag, size)));
            }
            if elements(&xml, "IsTruncated").next() != Some("true") {
                return Ok(parts);
            }
            marker = elements(&xml, "NextPartNumberMarker").next().map(unescape);
            if marker.as_deref().is_none_or(str::is_empty) {
                return Err(Error::Http(format!(
                    "PARTS {key}: truncated without a marker"
                )));
            }
        }
    }
}

impl<D: Dialect> Client<D> {
    /// A PUT of `body` with `headers`, conditional on `guard`: `None` when
    /// it failed and wrote nothing, as `written` judges the answer.
    async fn put_if(
        &self,
        key: &str,
        body: Payload,
        guard: Option<&Guard>,
        mut headers: Vec<(String, String)>,
    ) -> Result<Option<Sent>> {
        if let Some(guard) = guard {
            let header =
                D::precondition(guard).map_err(|e| Error::Http(format!("PUT {key}: {e}")))?;
            headers.push(header);
        }
        let put = call_for("PUT", guard.is_some(), &body, None);
        let sent = self.exchange(&put, key, &[], headers).await?;
        Ok(written(key, &sent, guard.is_some())?.then_some(sent))
    }

    /// The keys under `prefix`, and with `delimit` the common prefixes
    /// up to the next `/`.
    async fn listing(&self, prefix: &str, delimit: bool) -> Result<(Vec<Entry>, Vec<String>)> {
        let (mut entries, mut prefixes) = (Vec::new(), Vec::new());
        let mut token: Option<String> = None;
        loop {
            let mut query = vec![("list-type", "2".to_owned()), ("prefix", prefix.to_owned())];
            if delimit {
                query.push(("delimiter", "/".to_owned()));
            }
            if let Some(token) = token.take() {
                query.push(("continuation-token", token));
            }
            let empty = none();
            let list = call_for("GET", false, &empty, None);
            let response = self.send(&list, "", &query, Vec::new()).await?;
            if response.status != 200 {
                return Err(failure("LIST", prefix, &response));
            }
            token = listing_page(prefix, &response.body, &mut entries, &mut prefixes)?;
            if token.is_none() {
                return Ok((entries, prefixes));
            }
        }
    }

    /// Authorize and send one request (`exchange`), answering its response.
    async fn send(
        &self,
        call: &Call<'_>,
        key: &str,
        query: &[(&str, String)],
        headers: Vec<(String, String)>,
    ) -> Result<HttpResponse> {
        let sent = self.exchange(call, key, query, headers).await?;
        Ok(sent.response)
    }

    /// Authorize and send one request. `key` is the object key, or empty
    /// for a bucket-level call.
    ///
    /// A request the bucket refused under load is sent again after a pause:
    /// R2 answers 429 to more than about one write a second to one key, and
    /// an owner record is written twice in a row when an object reclaims:
    /// under a 10,000-object kill, four reclaims failed that way and dropped
    /// their objects. So is one whose connection failed before it answered
    /// (`http::Failure::Lost`, `http::Failure::Unsent`), which hyper does
    /// not send again itself once its dispatcher has taken it, nor on a new
    /// connection: a bucket closing an idle pooled connection as a request
    /// goes out on it fails the request that way, most often when this
    /// node's loop is too busy to have seen the close in time. So is a
    /// request refused as unauthenticated, once, when the dialect has
    /// another credential for it. Each authorized request is
    /// sent once more if `http::prompt` bounds it and it has not answered in
    /// time (`http::attempt`), both copies with one authorization, their
    /// answers judged by `settles`.
    ///
    /// An attempt sent again may have been applied, and a conditional
    /// write's next attempt then fails its guard against it
    /// (`Sent::maybe_applied`).
    async fn exchange(
        &self,
        call: &Call<'_>,
        key: &str,
        query: &[(&str, String)],
        headers: Vec<(String, String)>,
    ) -> Result<Sent> {
        let Call { method, .. } = *call;
        let what = || match query.iter().find(|(name, _)| *name == "prefix") {
            Some((_, prefix)) if key.is_empty() => format!("LIST {prefix}"),
            _ => format!("{method} {key}"),
        };
        let path = format!("/{}/{}", encode(&self.bucket, false), encode(key, false));
        let mut query: Vec<String> = query
            .iter()
            .map(|(name, value)| format!("{}={}", encode(name, true), encode(value, true)))
            .collect();
        query.sort();
        let query = query.join("&");
        let host = self
            .endpoint
            .split_once("://")
            .map_or(self.endpoint.as_str(), |(_, host)| host);
        let url = if query.is_empty() {
            format!("{}{path}", self.endpoint)
        } else {
            format!("{}{path}?{query}", self.endpoint)
        };
        let mut pause = self.backoff;
        let mut reauthorized = false;
        let mut maybe_applied = false;
        loop {
            let mut request = Request {
                method,
                host,
                path: &path,
                query: &query,
                headers: headers.clone(),
            };
            let credential = self.dialect.authorize(&mut request).await?;
            let once = || self.send_once(call, &url, request.headers.clone());
            let settled = |response: &HttpResponse| settles(call.guarded, response);
            let attempt = crate::http::attempt(call, what, settled, once).await;
            maybe_applied |= attempt.sent_twice;
            let response = match attempt.answer {
                Ok(response) => response,
                Err(error)
                    if error.failure != Failure::Other && pause <= self.backoff * MAX_BACKOFF =>
                {
                    tracing::warn!(
                        "cluster: {} got no answer: {error}; sending it again",
                        what()
                    );
                    maybe_applied |= error.failure == Failure::Lost;
                    tokio::time::sleep(pause).await;
                    pause *= 2;
                    continue;
                }
                Err(error) => return Err(request_error(method, key, &error.message)),
            };
            // A refused credential is dropped even when the request is not
            // sent again, so that no later request carries it.
            if response.status == 401
                && credential.is_some_and(|c| self.dialect.refused(&c))
                && !reauthorized
            {
                reauthorized = true;
                continue;
            }
            if !matches!(response.status, 429 | 503) || pause > self.backoff * MAX_BACKOFF {
                return Ok(Sent {
                    response,
                    maybe_applied,
                });
            }
            maybe_applied = true;
            tokio::time::sleep(pause).await;
            pause *= 2;
        }
    }

    async fn send_once(
        &self,
        call: &Call<'_>,
        url: &str,
        headers: Vec<(String, String)>,
    ) -> std::result::Result<HttpResponse, HttpError> {
        let (method, body) = (call.method, call.body.clone());
        let headers = headers.into_iter().flat_map(<[String; 2]>::from).collect();
        // A hung request would stall whatever waits on it: a tiering pass
        // waits for every object, so one could stop a node's tiering. A
        // large body gets a second per MiB on top, at least.
        let size = match &body {
            Payload::Bytes(bytes) => bytes.len() as u64,
            Payload::File(path) => std::fs::metadata(path).map_or(0, |m| m.len()),
        };
        let head = REQUEST_TIMEOUT + Duration::from_secs(size >> 20);
        let sent = SystemTime::now();
        let (url, bounds) = (url.to_owned(), (head, REQUEST_TIMEOUT));
        let response = if call.stream {
            self.http
                .open(method.into(), url, headers, body, bounds)
                .await
        } else {
            let to = call.to;
            self.http
                .send(method.into(), url, headers, body, to, bounds)
                .await
        };
        let response = response?;
        if let Some(skew) = skew_ms(sent, SystemTime::now(), &response.date) {
            self.skew_ms.set(Some(skew));
        }
        Ok(response)
    }
}

/// A request's response, after the attempts `Client::exchange` made of it.
struct Sent {
    response: HttpResponse,
    /// Whether an attempt before the one answered may have been applied, so
    /// that the bucket may have answered this one against it. One answered
    /// 429 or 503 may have been: a 503 may come from anywhere on the way,
    /// after the bucket applied the request, and that a store's 429 refuses
    /// before anything applies is that store's to promise, which not every
    /// S3-compatible one does. So may one whose connection failed after
    /// hyper took it (`http::Failure::Lost`): the bucket may have received
    /// all of it. Taking a refusal for a possible write costs a read; the
    /// reverse misreports the write. One answered 401 was refused first:
    /// the credential is checked before the request is processed. So may a
    /// copy `http::attempt` sent beside the one answered
    /// (`http::Attempt::sent_twice`), before that one's guard was checked
    /// or after: its own answer is dropped when the other's comes first, a
    /// 401 among them.
    maybe_applied: bool,
}

fn none() -> Payload {
    Payload::Bytes(bytes::Bytes::new())
}

/// A `method` request with `body`, a write with a precondition if `guarded`,
/// whose answer's body goes to the file `to`, if any.
const fn call_for<'a>(
    method: &'a str,
    guarded: bool,
    body: &'a Payload,
    to: Option<&'a Path>,
) -> Call<'a> {
    Call {
        method,
        guarded,
        body,
        to,
        stream: false,
    }
}

/// Whether `response` answers its request for good while another copy of
/// the request is in flight (`http::attempt`), rather than wait for that
/// one's answer. A refusal (429, 503) did nothing, and says nothing of the
/// other copy. A guarded write's failed guard (412) or concurrent write
/// (R2's 409) may be the other copy's own write, landed or in progress,
/// whose answer is then its success; without that answer, the write's
/// outcome is unknown (`Sent::maybe_applied`). S3 and Cloud Storage answer
/// alike.
const fn settles(guarded: bool, response: &HttpResponse) -> bool {
    match response.status {
        429 | 503 => false,
        409 | 412 => !guarded,
        _ => true,
    }
}

/// The pause before a request the bucket refused with a 429 or 503, or whose
/// connection failed before it answered, is sent again, doubled for each
/// such failure after it, up to `MAX_BACKOFF` times this: 250 ms, 0.5, 1 and
/// 2 s, after which the failure is the answer.
const BACKOFF: Duration = Duration::from_millis(250);
const MAX_BACKOFF: u32 = 8;

/// How long one bucket request may take. R2 answers in 0.1-1 s from the
/// lab, with spikes of a few seconds.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

fn failure(call: &str, key: &str, response: &HttpResponse) -> Error {
    let code = code(response);
    Error::Http(format!("{call} {key}: {} {code}", response.status))
}

/// `failure`, as `Error::Refused`.
fn refused(call: &str, key: &str, response: &HttpResponse) -> Error {
    crate::bucket::refused(format!("{call} {key}"), response.status, &code(response))
}

/// The error answer's `Code`, at most `MAX_CODE` of it.
fn code(response: &HttpResponse) -> String {
    let body = String::from_utf8_lossy(&response.body);
    let code = elements(&body, "Code").next().unwrap_or("");
    code.chars().take(MAX_CODE).collect()
}

/// The most of an error answer's `Code` that goes into a message.
const MAX_CODE: usize = 100;

/// Whether a PUT's answer says it wrote the object, `false` when its guard
/// failed and it wrote nothing. A guard that failed after an earlier attempt,
/// or a copy sent beside it, may have been applied may have failed against
/// that one's write: `Error::OutcomeUnknown`. A 409 or 412 to a write without
/// a guard is an error: `Store::put` answers `Ok(None)` only for a failed
/// guard.
fn written(key: &str, sent: &Sent, guarded: bool) -> Result<bool> {
    let response = &sent.response;
    match response.status {
        200 => Ok(true),
        // 412: the guard failed. 409: a concurrent conditional write to the
        // same key won the race, which is the same answer.
        status @ (409 | 412) if guarded && sent.maybe_applied => Err(Error::OutcomeUnknown(
            format!("PUT {key}: {status} to a retry"),
        )),
        409 | 412 if guarded => Ok(false),
        _ => Err(refused("PUT", key, response)),
    }
}

/// The version a successful `call` on `key` answered. An answer without one
/// is an error: the version guards the next write, and an empty precondition
/// is one a store may ignore. So it is for calls that discard the version
/// (`Bucket::get_file`, `put`, `put_file`): a store that omits versions can
/// fence nothing, and refusing every such answer has a node refuse it when
/// it joins (`check_conditional_writes` fails), not partway through, at its
/// first conditional write.
fn version<D: Dialect>(call: &str, key: &str, response: &HttpResponse) -> Result<Version> {
    match D::version(response) {
        "" => Err(Error::Http(format!(
            "{call} {key}: {} without a version ({})",
            response.status,
            D::VERSION
        ))),
        version => Ok(Version(version.to_owned())),
    }
}

/// `SigV4` URI encoding: everything but unreserved characters, and `/` too
/// unless the value is a path.
pub fn encode(value: &str, slash: bool) -> String {
    value.bytes().fold(String::new(), |mut out, byte| {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) || (byte == b'/' && !slash) {
            out.push(char::from(byte));
        } else {
            let _ = write!(out, "%{byte:02X}");
        }
        out
    })
}

/// Adds a `ListObjectsV2` answer's keys to `entries` and its common prefixes to
/// `prefixes`; answers the continuation token when the listing of `prefix`
/// goes on. A truncated answer without a token is an error: a listing that
/// stopped there would look complete.
fn listing_page(
    prefix: &str,
    body: &[u8],
    entries: &mut Vec<Entry>,
    prefixes: &mut Vec<String>,
) -> Result<Option<String>> {
    let xml = String::from_utf8_lossy(body);
    for contents in elements(&xml, "Contents") {
        let text = |name| elements(contents, name).next().map(unescape);
        entries.extend(text("Key").map(|key| {
            Entry {
                key,
                size: text("Size").and_then(|size| size.parse().ok()).unwrap_or(0),
                etag: unquote(&text("ETag").unwrap_or_default()),
                // Cloud Storage lists each object's generation; on S3 the
                // `ETag` is the version.
                version: Version(
                    text("Generation")
                        .or_else(|| text("ETag"))
                        .unwrap_or_default(),
                ),
                modified_ms: text("LastModified").and_then(|t| iso_ms(&t)).unwrap_or(0),
            }
        }));
    }
    for common in elements(&xml, "CommonPrefixes") {
        prefixes.extend(elements(common, "Prefix").next().map(unescape));
    }
    if elements(&xml, "IsTruncated").next() != Some("true") {
        return Ok(None);
    }
    match elements(&xml, "NextContinuationToken").next() {
        Some(next) if !next.is_empty() => Ok(Some(unescape(next))),
        _ => Err(Error::Http(format!(
            "LIST {prefix}: truncated without a continuation token"
        ))),
    }
}

/// How far this clock is outside the bucket's, from a response's `Date`
/// header, for a request sent at `sent` and answered at `received`.
fn skew_ms(sent: SystemTime, received: SystemTime, date: &str) -> Option<i64> {
    let date = http_date_ms(date)?;
    // The bucket stamped the header, in whole seconds, between the send and
    // the answer: this clock is off only by what lies outside that window.
    // Measured from the send alone, a write R2 held 8 s at its 5-minute mark
    // read as a clock 8 s slow.
    let (sent, received) = (unix_ms(sent), unix_ms(received));
    Some(if received < date {
        received - date
    } else if sent > date + 1000 {
        sent - date - 1000
    } else {
        0
    })
}

/// The text of each `<name>…</name>` element, in order. S3's listing XML has
/// no attributes on the elements read here and no nesting of equal names.
fn elements<'a>(xml: &'a str, name: &str) -> impl Iterator<Item = &'a str> {
    let open = format!("<{name}>");
    let close = format!("</{name}>");
    let mut rest = xml;
    std::iter::from_fn(move || {
        let start = rest.find(&open)? + open.len();
        let end = start + rest[start..].find(&close)?;
        let text = &rest[start..end];
        rest = &rest[end + close.len()..];
        Some(text)
    })
}

/// The object a HEAD or a GET answered. Its size is the whole object's: of a
/// 206, the one `Content-Range` gives. Cloud Storage answers a stored size
/// apart, `x-goog-stored-content-length`, which is the one that counts
/// where `Content-Length` may differ.
fn object<D: Dialect>(call: &str, key: &str, response: &HttpResponse) -> Result<Object> {
    let header = |name: &str| {
        let value = response.headers.get(name)?;
        Some(String::from_utf8_lossy(value.as_bytes()).into_owned())
    };
    let range = header("content-range").and_then(|range| content_range(&range));
    let size = match range {
        Some((_, _, size)) => Some(size),
        None if response.status == 206 => None,
        None => header("x-goog-stored-content-length")
            .or_else(|| header("content-length"))
            .and_then(|size| size.parse().ok()),
    };
    let size = size
        .ok_or_else(|| Error::Http(format!("{call} {key}: {} without a size", response.status)))?;
    let metadata = response.headers.iter().filter_map(|(name, value)| {
        let name = name.as_str().strip_prefix(D::METADATA)?;
        Some((
            name.to_owned(),
            String::from_utf8_lossy(value.as_bytes()).into_owned(),
        ))
    });
    let modified = header("last-modified").and_then(|date| http_date_ms(&date));
    Ok(Object {
        size,
        version: version::<D>(call, key, response)?,
        etag: unquote(&response.etag),
        modified_ms: modified.and_then(|ms| u64::try_from(ms).ok()).unwrap_or(0),
        headers: Headers::from_fn(header),
        metadata: metadata.collect(),
        range: range.map(|(offset, length, _)| (offset, length)),
    })
}

/// `bytes <first>-<last>/<size>` as offset, length and size.
fn content_range(range: &str) -> Option<(u64, u64, u64)> {
    let (span, size) = range.strip_prefix("bytes ")?.split_once('/')?;
    let (first, last) = span.split_once('-')?;
    let (first, last): (u64, u64) = (first.parse().ok()?, last.parse().ok()?);
    let len = last.checked_add(1)?.checked_sub(first)?;
    Some((first, len, size.parse().ok()?))
}

/// The headers of a write's content headers and user metadata.
fn described<D: Dialect>(
    headers: &Headers,
    metadata: &[(String, String)],
) -> Vec<(String, String)> {
    let content = headers.named().into_iter();
    let content = content.filter_map(|(name, value)| Some((name.to_owned(), value?.to_owned())));
    let metadata = metadata
        .iter()
        .map(|(name, value)| (format!("{}{name}", D::METADATA), value.clone()));
    content.chain(metadata).collect()
}

/// `Dialect::READ`, as headers.
fn reading<D: Dialect>() -> Vec<(String, String)> {
    D::READ
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect()
}

/// An `ETag` without its quotes.
fn unquote(etag: &str) -> String {
    etag.trim_matches('"').to_owned()
}

/// A listing's time, `2026-10-12T23:10:55.123Z`, in ms since the epoch.
fn iso_ms(time: &str) -> Option<u64> {
    let (date, time) = time.strip_suffix('Z')?.split_once('T')?;
    let (time, fraction) = time.split_once('.').unwrap_or((time, "0"));
    let numbers = |text: &str, separator| {
        let mut parts = text.split(separator).map(|n| n.parse::<i64>().ok());
        Some((parts.next()??, parts.next()??, parts.next()??))
    };
    let ((year, month, day), (h, m, s)) = (numbers(date, '-')?, numbers(time, ':')?);
    let ms: i64 = format!("{fraction:0<3}").get(..3)?.parse().ok()?;
    // Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
    let year = if month <= 2 { year - 1 } else { year };
    let era = year.div_euclid(400);
    let yoe = year - era * 400;
    let doy = (153 * (month + if month > 2 { -3 } else { 9 }) + 2) / 5 + day - 1;
    let days = era * 146_097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719_468;
    u64::try_from((days * 86_400 + h * 3600 + m * 60 + s) * 1000 + ms).ok()
}

/// `text` with its XML entities decoded, numeric ones too: S3 writes a key's
/// control characters as `&#13;`.
fn unescape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(at) = rest.find('&') {
        out.push_str(&rest[..at]);
        rest = &rest[at..];
        let entity = rest
            .find(';')
            .and_then(|end| Some((entity(&rest[1..end])?, end)));
        let Some((c, end)) = entity else {
            out.push('&');
            rest = &rest[1..];
            continue;
        };
        out.push(c);
        rest = &rest[end + 1..];
    }
    out + rest
}

fn entity(name: &str) -> Option<char> {
    let n = match name {
        "lt" => '<'.into(),
        "gt" => '>'.into(),
        "quot" => '"'.into(),
        "apos" => '\''.into(),
        "amp" => '&'.into(),
        _ => match name.strip_prefix("#x") {
            Some(hex) => u32::from_str_radix(hex, 16).ok()?,
            None => name.strip_prefix('#')?.parse().ok()?,
        },
    };
    char::from_u32(n)
}

fn unix_ms(t: SystemTime) -> i64 {
    i64::try_from(t.duration_since(UNIX_EPOCH).unwrap_or_default().as_millis()).unwrap_or(0)
}

/// An RFC 7231 date, `Mon, 12 Oct 2026 23:10:55 GMT`, in ms since the epoch.
fn http_date_ms(date: &str) -> Option<i64> {
    httpdate::parse_http_date(date).ok().map(unix_ms)
}

/// Converts `error` from a request for `key` to a bucket error.
fn request_error(method: &str, key: &str, error: &str) -> Error {
    if error == crate::http::TOO_LARGE {
        Error::TooLarge(format!("{method} {key}"))
    } else {
        Error::Http(format!("{method} {key}: {error}"))
    }
}

#[cfg(test)]
#[path = "client-test.rs"]
mod tests;
