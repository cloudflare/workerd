// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! S3's dialect of the bucket client (`client.rs`).
//!
//! Requests are signed with `SigV4`, versions are `ETag`s, and preconditions
//! are `If-None-Match` and `If-Match`. S3, R2 and `MinIO` all accept the
//! client's path-style URLs.

use std::cell::RefCell;
use std::path::PathBuf;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use crate::Error;
use crate::Result;
use crate::bucket::Guard;
use crate::bucket::Version;
use crate::client::Client;
use crate::client::Dialect;
use crate::client::Request;
use crate::hex;
use crate::hmac_sha256;
use crate::http::Http;
use crate::http::HttpResponse;
use crate::sha256;

/// An S3 bucket's client.
pub type S3 = Client<SigV4>;

/// Signs requests with `SigV4`.
pub struct SigV4 {
    region: String,
    credentials: RefCell<Credentials>,
    /// `aws_credentials_file`, reread when its modification time changes.
    file: Option<PathBuf>,
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
        let required = |name: &str| {
            option(name).ok_or_else(|| {
                let var = name.to_uppercase();
                Error::Config(format!("set {name} or {var}"))
            })
        };
        let region = option("aws_region").unwrap_or_else(|| "us-east-1".into());
        let endpoint =
            option("aws_endpoint").unwrap_or_else(|| format!("https://s3.{region}.amazonaws.com"));
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
        let http = Http::new().map_err(|e| Error::Config(e.to_string()))?;
        let signer = SigV4 {
            region,
            credentials: RefCell::new(credentials),
            file,
        };
        Ok((
            Self::with_dialect(signer, http, &endpoint, bucket),
            prefix.to_owned(),
        ))
    }
}

impl Dialect for SigV4 {
    const METADATA: &'static str = "x-amz-meta-";
    const VERSION: &'static str = "ETag";

    fn version(response: &HttpResponse) -> &str {
        &response.etag
    }

    fn precondition(guard: &Guard) -> (String, String) {
        match guard {
            Guard::Absent => ("if-none-match".into(), "*".into()),
            Guard::Match(Version(etag)) => ("if-match".into(), etag.clone()),
        }
    }

    async fn authorize(&self, request: &mut Request<'_>) -> Result<()> {
        let Request {
            method,
            host,
            path,
            query,
            ..
        } = *request;
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
        request.headers.extend(signed);
        request.headers.push((
            "authorization".into(),
            format!(
                "AWS4-HMAC-SHA256 Credential={}/{scope}, SignedHeaders={names}, Signature={signature}",
                credentials.key_id
            ),
        ));
        Ok(())
    }
}

impl SigV4 {
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
#[path = "s3-test.rs"]
mod tests;
