// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The four S3 calls the cluster makes, signed with `SigV4`. Requests use
//! path-style URLs, which S3, R2 and `MinIO` all accept.

use std::cell::Cell;
use std::cell::RefCell;
use std::fmt::Write;
use std::path::Path;
use std::path::PathBuf;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use crate::Error;
use crate::Result;
use crate::bucket::Entry;
use crate::bucket::Guard;
use crate::bucket::Store;
use crate::bucket::Version;
use crate::hex;
use crate::hmac_sha256;
use crate::http::Http;
use crate::http::HttpResponse;
use crate::http::Payload;
use crate::sha256;

pub struct S3 {
    http: Http,
    endpoint: String,
    bucket: String,
    region: String,
    credentials: RefCell<Credentials>,
    /// `aws_credentials_file`, reread when its modification time changes.
    file: Option<PathBuf>,
    /// This node's wall clock less the bucket's, at the last response.
    skew_ms: Cell<Option<i64>>,
}

#[derive(Clone, Default)]
struct Credentials {
    key_id: String,
    secret: String,
    token: Option<String>,
    modified: Option<SystemTime>,
}

impl Credentials {
    /// `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`
    /// lines, as an env file writes them.
    fn parse(text: &str, modified: Option<SystemTime>) -> Option<Self> {
        let mut out = Self {
            modified,
            ..Self::default()
        };
        for line in text.lines() {
            let line = line.trim().trim_start_matches("export ");
            let Some((name, value)) = line.split_once('=') else {
                continue;
            };
            let value = value.trim().trim_matches(['\'', '"']).to_owned();
            match name.trim() {
                "AWS_ACCESS_KEY_ID" => out.key_id = value,
                "AWS_SECRET_ACCESS_KEY" => out.secret = value,
                "AWS_SESSION_TOKEN" => out.token = Some(value),
                _ => {}
            }
        }
        (!out.key_id.is_empty() && !out.secret.is_empty()).then_some(out)
    }
}

impl S3 {
    /// `url` is `s3://<bucket>/<prefix>`; answers the client and the prefix.
    /// Options are `aws_endpoint`, `aws_region`, `aws_access_key_id`,
    /// `aws_secret_access_key` and `aws_token`, or in place of the last three
    /// `aws_credentials_file`, so that rotating short-lived credentials
    /// needs no restart. Each defaults to its AWS environment variable
    /// (`AWS_ENDPOINT_URL`, `AWS_SESSION_TOKEN`, else upper-cased).
    pub fn new(url: &str, options: &[(String, String)]) -> Result<(Self, String)> {
        let rest = url
            .strip_prefix("s3://")
            .ok_or_else(|| Error::Config(format!("{url}: expected s3://<bucket>/<prefix>")))?;
        let (bucket, prefix) = rest.split_once('/').unwrap_or((rest, ""));
        // An option missing from the config falls back to the environment
        // variable the AWS tools read.
        let option = |name: &str| {
            let var = match name {
                "aws_endpoint" => "AWS_ENDPOINT_URL".to_owned(),
                "aws_token" => "AWS_SESSION_TOKEN".to_owned(),
                _ => name.to_uppercase(),
            };
            options
                .iter()
                .find(|(key, _)| key == name)
                .map(|(_, value)| value.clone())
                .or_else(|| std::env::var(var).ok())
        };
        let required = |name: &str| option(name).ok_or_else(|| Error::Config(name.to_owned()));
        let region = option("aws_region").unwrap_or_else(|| "us-east-1".into());
        let endpoint = option("aws_endpoint")
            .unwrap_or_else(|| format!("https://s3.{region}.amazonaws.com"))
            .trim_end_matches('/')
            .to_owned();
        let file = option("aws_credentials_file").map(PathBuf::from);
        let credentials = match &file {
            Some(path) => read_credentials(path)
                .ok_or_else(|| Error::Config(format!("{}: no credentials", path.display())))?,
            None => Credentials {
                key_id: required("aws_access_key_id")?,
                secret: required("aws_secret_access_key")?,
                token: option("aws_token"),
                modified: None,
            },
        };
        Ok((
            Self {
                http: Http::new().map_err(|e| Error::Config(e.to_string()))?,
                endpoint,
                bucket: bucket.to_owned(),
                region,
                credentials: RefCell::new(credentials),
                file,
                skew_ms: Cell::new(None),
            },
            prefix.to_owned(),
        ))
    }
}

#[async_trait::async_trait(?Send)]
impl Store for S3 {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        let response = self.send("GET", key, &[], Vec::new(), none(), to).await?;
        match response.status {
            200 => Ok(Some((response.body, Version(response.etag)))),
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
            .map(|(name, value)| (format!("x-amz-meta-{name}"), value.clone()))
            .collect();
        match guard {
            None => {}
            Some(Guard::Absent) => headers.push(("if-none-match".into(), "*".into())),
            Some(Guard::Match(Version(etag))) => headers.push(("if-match".into(), etag)),
        }
        let response = self.send("PUT", key, &[], headers, body, None).await?;
        match response.status {
            200 => Ok(Some(Version(response.etag))),
            // 412: the guard failed. 409: a concurrent conditional write to
            // the same key won the race, which is the same answer.
            409 | 412 => Ok(None),
            _ => Err(failure("PUT", key, &response)),
        }
    }

    async fn delete(&self, key: &str) -> Result<()> {
        let response = self
            .send("DELETE", key, &[], Vec::new(), none(), None)
            .await?;
        match response.status {
            200 | 204 | 404 => Ok(()),
            _ => Err(failure("DELETE", key, &response)),
        }
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        let mut entries = Vec::new();
        let mut token: Option<String> = None;
        loop {
            let mut query = vec![("list-type", "2".to_owned()), ("prefix", prefix.to_owned())];
            if let Some(token) = token.take() {
                query.push(("continuation-token", token));
            }
            let response = self
                .send("GET", "", &query, Vec::new(), none(), None)
                .await?;
            if response.status != 200 {
                return Err(failure("LIST", prefix, &response));
            }
            let xml = String::from_utf8_lossy(&response.body);
            for contents in elements(&xml, "Contents") {
                let key = elements(contents, "Key").next().map(unescape);
                entries.extend(key.map(|key| Entry { key }));
            }
            match elements(&xml, "NextContinuationToken").next() {
                Some(next) if elements(&xml, "IsTruncated").next() == Some("true") => {
                    token = Some(unescape(next));
                }
                _ => return Ok(entries),
            }
        }
    }

    /// How far this node's wall clock is outside the bucket's, from the last
    /// response's `Date` header: 0 when the header fits the request's window.
    fn clock_skew_ms(&self) -> Option<i64> {
        self.skew_ms.get()
    }
}

impl S3 {
    /// Sign and send one request. `key` is the object key, or empty for a
    /// bucket-level call.
    /// R2 answers 429 to more than about one write a second to one key, and
    /// an owner record is written twice in a row when an object reclaims:
    /// under a 10,000-object kill, four reclaims failed that way and dropped
    /// their objects. A refused request did nothing, conditional or not, so
    /// it is retried after a pause.
    async fn send(
        &self,
        method: &str,
        key: &str,
        query: &[(&str, String)],
        headers: Vec<(String, String)>,
        body: Payload,
        to: Option<&Path>,
    ) -> Result<HttpResponse> {
        let mut pause = Duration::from_millis(250);
        loop {
            let response = self
                .send_once(method, key, query, headers.clone(), body.clone(), to)
                .await?;
            if !matches!(response.status, 429 | 503) || pause > Duration::from_secs(2) {
                return Ok(response);
            }
            tokio::time::sleep(pause).await;
            pause *= 2;
        }
    }

    async fn send_once(
        &self,
        method: &str,
        key: &str,
        query: &[(&str, String)],
        mut headers: Vec<(String, String)>,
        body: Payload,
        to: Option<&Path>,
    ) -> Result<HttpResponse> {
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
        self.refresh();
        let credentials = self.credentials.borrow().clone();
        let (date, time) = amz_date(SystemTime::now());
        // The payload is not hashed: TLS protects it, and hashing would mean
        // a second pass over every LTX file.
        let mut signed = vec![
            ("host".to_owned(), host.to_owned()),
            (
                "x-amz-content-sha256".to_owned(),
                "UNSIGNED-PAYLOAD".to_owned(),
            ),
            ("x-amz-date".to_owned(), time.clone()),
        ];
        if let Some(token) = &credentials.token {
            signed.push(("x-amz-security-token".to_owned(), token.clone()));
        }
        signed.sort();
        let names = signed
            .iter()
            .map(|(name, _)| name.as_str())
            .collect::<Vec<_>>()
            .join(";");
        let canonical = format!(
            "{method}\n{path}\n{query}\n{}\n{names}\nUNSIGNED-PAYLOAD",
            signed.iter().fold(String::new(), |out, (name, value)| out
                + name
                + ":"
                + value
                + "\n"),
        );
        let scope = format!("{date}/{}/s3/aws4_request", self.region);
        let to_sign = format!(
            "AWS4-HMAC-SHA256\n{time}\n{scope}\n{}",
            hex(&sha256(canonical.as_bytes()))
        );
        let mut signing = format!("AWS4{}", credentials.secret).into_bytes();
        for part in [date.as_str(), &self.region, "s3", "aws4_request"] {
            signing = hmac_sha256(&signing, part.as_bytes());
        }
        let signature = hex(&hmac_sha256(&signing, to_sign.as_bytes()));
        headers.extend(signed);
        headers.push((
            "authorization".into(),
            format!(
                "AWS4-HMAC-SHA256 Credential={}/{scope}, SignedHeaders={names}, Signature={signature}",
                credentials.key_id
            ),
        ));

        let url = if query.is_empty() {
            format!("{}{path}", self.endpoint)
        } else {
            format!("{}{path}?{query}", self.endpoint)
        };
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
                url,
                headers,
                body,
                to,
                (head, REQUEST_TIMEOUT),
            )
            .await
            .map_err(|error| Error::Http(format!("{method} {key}: {error}")))?;
        if let Some(date) = http_date_ms(&response.date) {
            // The bucket stamped the header, in whole seconds, between the
            // send and the answer: this clock is off only by what lies
            // outside that window. Measured from the send alone, a write
            // R2 held 8 s at its 5-minute mark read as a clock 8 s slow.
            let (sent, received) = (unix_ms(sent), unix_ms(SystemTime::now()));
            let skew = if received < date {
                received - date
            } else if sent > date + 1000 {
                sent - date - 1000
            } else {
                0
            };
            self.skew_ms.set(Some(skew));
        }
        Ok(response)
    }

    /// Rereads the credentials file if it changed. A file caught mid-write
    /// or emptied keeps the credentials in use.
    fn refresh(&self) {
        let Some(path) = &self.file else {
            return;
        };
        let modified = std::fs::metadata(path).and_then(|m| m.modified()).ok();
        if modified.is_none() || modified == self.credentials.borrow().modified {
            return;
        }
        if let Some(credentials) = read_credentials(path) {
            *self.credentials.borrow_mut() = credentials;
        }
    }
}

fn read_credentials(path: &std::path::Path) -> Option<Credentials> {
    let modified = std::fs::metadata(path).and_then(|m| m.modified()).ok();
    Credentials::parse(&std::fs::read_to_string(path).ok()?, modified)
}

fn none() -> Payload {
    Payload::Bytes(bytes::Bytes::new())
}

/// How long one bucket request may take. R2 answers in 0.1-1 s from the
/// lab, with spikes of a few seconds.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

fn failure(call: &str, key: &str, response: &HttpResponse) -> Error {
    let body = String::from_utf8_lossy(&response.body);
    let code = elements(&body, "Code").next().unwrap_or("");
    Error::Http(format!("{call} {key}: {} {code}", response.status))
}

/// `SigV4` URI encoding: everything but unreserved characters, and `/` too
/// unless the value is a path.
fn encode(value: &str, slash: bool) -> String {
    value.bytes().fold(String::new(), |mut out, byte| {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) || (byte == b'/' && !slash) {
            out.push(char::from(byte));
        } else {
            let _ = write!(out, "%{byte:02X}");
        }
        out
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

/// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
fn civil(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = u32::try_from(doy - (153 * mp + 2) / 5 + 1).unwrap_or(1);
    let month = u32::try_from(if mp < 10 { mp + 3 } else { mp - 9 }).unwrap_or(1);
    (yoe + era * 400 + i64::from(month <= 2), month, day)
}

fn unix_ms(t: SystemTime) -> i64 {
    i64::try_from(t.duration_since(UNIX_EPOCH).unwrap_or_default().as_millis()).unwrap_or(0)
}

/// An RFC 7231 date, `Mon, 12 Oct 2026 23:10:55 GMT`, in ms since the epoch.
fn http_date_ms(date: &str) -> Option<i64> {
    httpdate::parse_http_date(date).ok().map(unix_ms)
}

/// `(YYYYMMDD, YYYYMMDDTHHMMSSZ)`.
fn amz_date(now: SystemTime) -> (String, String) {
    let secs =
        i64::try_from(now.duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()).unwrap_or(0);
    let (year, month, day) = civil(secs.div_euclid(86_400));
    let s = secs.rem_euclid(86_400);
    let date = format!("{year:04}{month:02}{day:02}");
    let time = format!("{date}T{:02}{:02}{:02}Z", s / 3600, s % 3600 / 60, s % 60);
    (date, time)
}

/// Unix milliseconds as Go's `time.RFC3339Nano` prints them: trailing zeros
/// of the fraction dropped, and no fraction at all when it is zero.
pub fn format_rfc3339_nano(unix_millis: i64) -> String {
    let millis = unix_millis.max(0);
    let secs = millis / 1000;
    let (year, month, day) = civil(secs / 86_400);
    let s = secs % 86_400;
    let mut out = format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}",
        s / 3600,
        s % 3600 / 60,
        s % 60
    );
    let fraction = millis % 1000;
    if fraction != 0 {
        let digits = format!("{fraction:03}");
        out = out + "." + digits.trim_end_matches('0');
    }
    out + "Z"
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    #[test]
    fn dates_round_trip() {
        let t = UNIX_EPOCH + Duration::from_millis(1_790_348_414_500);
        assert_eq!(
            format_rfc3339_nano(1_790_348_414_500),
            "2026-09-25T15:00:14.5Z"
        );
        assert_eq!(
            format_rfc3339_nano(1_790_348_414_000),
            "2026-09-25T15:00:14Z"
        );
        assert_eq!(amz_date(t).1, "20260925T150014Z");
        assert_eq!(civil(11_016), (2000, 2, 29));
    }

    #[test]
    fn http_dates() {
        assert_eq!(
            http_date_ms("Mon, 12 Oct 2026 23:10:55 GMT"),
            Some(1_791_846_655_000)
        );
        assert_eq!(
            http_date_ms("Tue, 29 Feb 2000 00:00:00 GMT"),
            Some(951_782_400_000)
        );
        assert_eq!(http_date_ms("nonsense"), None);
    }

    #[test]
    fn credentials_from_an_env_file() {
        let c = Credentials::parse(
            "# r2\nexport AWS_ACCESS_KEY_ID='k'\nAWS_SECRET_ACCESS_KEY=\"s\"\n\
             AWS_SESSION_TOKEN=t=\nS3_ENDPOINT=https://x\n",
            None,
        )
        .unwrap();
        assert_eq!(
            (&*c.key_id, &*c.secret, c.token.as_deref()),
            ("k", "s", Some("t="))
        );
        assert!(Credentials::parse("AWS_ACCESS_KEY_ID=k\n", None).is_none());
    }

    #[test]
    fn listing_xml() {
        let xml = "<ListBucketResult><IsTruncated>true</IsTruncated>\
            <Contents><Key>a&amp;b</Key><Size>3</Size></Contents>\
            <NextContinuationToken>t</NextContinuationToken></ListBucketResult>";
        let keys: Vec<_> = elements(xml, "Contents")
            .map(|c| unescape(elements(c, "Key").next().unwrap()))
            .collect();
        assert_eq!(keys, ["a&b"]);
        assert_eq!(elements(xml, "NextContinuationToken").next(), Some("t"));
    }

    #[test]
    fn sigv4_encoding() {
        assert_eq!(encode("ns/a b+c", false), "ns/a%20b%2Bc");
        assert_eq!(encode("ns/a", true), "ns%2Fa");
    }
}
