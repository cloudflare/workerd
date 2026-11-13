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

use crate::Error;
use crate::Result;
use crate::bucket::Entry;
use crate::bucket::Guard;
use crate::bucket::Store;
use crate::bucket::Version;
use crate::http::Call;
use crate::http::Http;
use crate::http::HttpResponse;
use crate::http::Payload;

/// What a bucket API's dialect decides about a request.
pub trait Dialect {
    /// The prefix of the headers that carry an object's user metadata.
    const METADATA: &'static str;

    /// The name of the header that carries an object's version.
    const VERSION: &'static str;

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
}

impl<D> Client<D> {
    pub fn with_dialect(dialect: D, http: Http, endpoint: &str, bucket: &str) -> Self {
        Self {
            dialect,
            http,
            endpoint: endpoint.trim_end_matches('/').to_owned(),
            bucket: bucket.to_owned(),
            skew_ms: Cell::new(None),
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
        let mut headers: Vec<(String, String)> = metadata
            .iter()
            .map(|(name, value)| (format!("{}{name}", D::METADATA), value.clone()))
            .collect();
        if let Some(guard) = &guard {
            let header =
                D::precondition(guard).map_err(|e| Error::Http(format!("PUT {key}: {e}")))?;
            headers.push(header);
        }
        let put = call_for("PUT", guard.is_some(), &body, None);
        let sent = self.exchange(&put, key, &[], headers).await?;
        if !written(key, &sent, guard.is_some())? {
            return Ok(None);
        }
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
}

impl<D: Dialect> Client<D> {
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
    /// their objects. So is a request refused as unauthenticated, once, when
    /// the dialect has another credential for it. Each authorized request is
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
        let Call {
            method,
            guarded,
            body,
            to,
        } = *call;
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
        let mut pause = Duration::from_millis(250);
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
            let once = || {
                let headers = request.headers.clone();
                self.send_once(method, key, &url, headers, body.clone(), to)
            };
            let settled = |response: &HttpResponse| settles(guarded, response);
            let attempt = crate::http::attempt(call, what, settled, once).await;
            maybe_applied |= attempt.sent_twice;
            let response = attempt.answer?;
            // A refused credential is dropped even when the request is not
            // sent again, so that no later request carries it.
            if response.status == 401
                && credential.is_some_and(|c| self.dialect.refused(&c))
                && !reauthorized
            {
                reauthorized = true;
                continue;
            }
            if !matches!(response.status, 429 | 503) || pause > Duration::from_secs(2) {
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
        method: &str,
        key: &str,
        url: &str,
        headers: Vec<(String, String)>,
        body: Payload,
        to: Option<&Path>,
    ) -> Result<HttpResponse> {
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
        let response = self
            .http
            .send(
                method.into(),
                url.to_owned(),
                headers,
                body,
                to,
                (head, REQUEST_TIMEOUT),
            )
            .await
            .map_err(|error| request_error(method, key, &error))?;
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
    /// S3-compatible one does. Taking a refusal for a possible write costs a
    /// read; the reverse misreports the write. One answered 401 was refused
    /// first: the credential is checked before the request is processed.
    /// So may a copy `http::attempt` sent beside the one answered
    /// (`http::Attempt::sent_twice`), before that one's guard was checked or
    /// after: its own answer is dropped when the other's comes first, a 401
    /// among them.
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

/// How long one bucket request may take. R2 answers in 0.1-1 s from the
/// lab, with spikes of a few seconds.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

fn failure(call: &str, key: &str, response: &HttpResponse) -> Error {
    let body = String::from_utf8_lossy(&response.body);
    let code = elements(&body, "Code").next().unwrap_or("");
    let code: String = code.chars().take(MAX_CODE).collect();
    Error::Http(format!("{call} {key}: {} {code}", response.status))
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
        _ => Err(failure("PUT", key, response)),
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
        let key = elements(contents, "Key").next().map(unescape);
        entries.extend(key.map(|key| Entry { key }));
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

fn unescape(text: &str) -> String {
    text.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
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
