// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `bucket` service with `r2` set: its objects as an R2 bucket, on the
//! HTTP protocol workerd's R2 binding speaks (`api/r2-rpc.c++`,
//! `api/r2-api.capnp`). GET carries the request as JSON in `CF-R2-Request`
//! for head, get and list; PUT carries it as a JSON prefix of the body,
//! `CF-R2-Metadata-Size` bytes long, for the rest. An answer's JSON metadata
//! prefixes its body, as long as its `CF-R2-Metadata-Size`; an error is
//! `CF-R2-Error`.
//!
//! Each R2 object is one store object under the service's path, with no
//! index, so the store's listing is the bucket's and a tool reading the
//! store sees the objects under their names. Content fields that are
//! header-safe are stored as headers; the rest, customMetadata, cacheExpiry,
//! checksums, the storage class and the upload time are one JSON record,
//! escaped to ASCII, in the user metadata `workerd-r2`. An object without it
//! reads its user metadata as its customMetadata.
//!
//! A put is read a part at a time, `PART` bytes, and hashed as it is read:
//! one guarded write of the first part when it is all, after its checksums
//! pass; else a multipart upload, completed only if they pass. Where a plain
//! object store cannot be R2:
//!
//! - a put over `PART` bytes has the etag `md5(md5s)-N` of a multipart
//!   upload, and its `onlyIf` is checked when it starts, not fenced;
//! - a part's etag is its MD5, so the same bytes uploaded twice under one
//!   part number have one etag, and either is accepted at completion;
//! - aborting an upload the store doesn't know succeeds;
//! - completing an upload with no parts writes an empty object without the
//!   upload's metadata;
//! - a part is held in memory, so one over `MAX_PART` bytes is refused;
//! - the record is user metadata, which S3 caps at 2 KB, so on S3 a put
//!   with more customMetadata than fits is refused as `MetadataTooLarge`.
//!
//! Limits and error messages follow Miniflare's R2 simulator. Not
//! supported, as there: SSE-C keys (ignored), bucket administration,
//! lifecycle rules.

use std::collections::BTreeMap;
use std::pin::Pin;

use bytes::Bytes;
use cluster::Body;
use cluster::Guard;
use cluster::Headers as Content;
use cluster::Md5;
use cluster::Object;
use cluster::Payload;
use cluster::Range;
use cluster::Version;
use cluster::hex;
use cluster::md5;
use futures::StreamExt;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use serde::Deserialize;
use serde::Serialize;
use serde_json::Map;
use serde_json::Value;
use serde_json::json;

use super::Shared;
use crate::Result;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::header_table;

/// A put is stored a part this large at a time.
const PART: usize = 8 << 20;
/// The largest part `uploadPart` takes, which it holds in memory; R2 takes
/// up to 5 GiB.
const MAX_PART: usize = 64 << 20;
const MAX_KEY: usize = 1024;
const MAX_VALUE: u64 = (5 << 30) - (5 << 20);
const MAX_METADATA: usize = 8192;
const MAX_LIST: u64 = 1000;
/// Store requests in flight at once for a listing's metadata or a delete
/// of many keys.
const PARALLEL: usize = 16;
/// The user metadata holding the R2 record.
const ENVELOPE: &str = "workerd-r2";
/// Before the store's token in a list cursor.
const CURSOR: &str = "t.";
/// Before the key a list cursor starts after, when a reply stopped short.
const AFTER: &str = "k.";
/// The most a list reply's JSON takes; the binding refuses over 1 MiB.
const MAX_REPLY: usize = 512 << 10;

/// The content fields and the headers that store them.
const FIELDS: [(&str, &str); 5] = [
    ("contentType", "content-type"),
    ("contentLanguage", "content-language"),
    ("contentDisposition", "content-disposition"),
    ("contentEncoding", "content-encoding"),
    ("cacheControl", "cache-control"),
];

/// The checksums, by their index in `R2Checksums`.
const ALGORITHMS: [(&str, &str); 5] = [
    ("md5", "MD5"),
    ("sha1", "SHA-1"),
    ("sha256", "SHA-256"),
    ("sha384", "SHA-384"),
    ("sha512", "SHA-512"),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Code {
    InternalError,
    NoSuchKey,
    EntityTooLarge,
    EntityTooSmall,
    MetadataTooLarge,
    InvalidObjectName,
    InvalidMaxKeys,
    NoSuchUpload,
    InvalidPart,
    InvalidMetadata,
    PreconditionFailed,
    BadDigest,
    InvalidRange,
    BadUpload,
}

impl Code {
    /// Its status, v4 code and message.
    #[rustfmt::skip]
    const fn info(self) -> (u16, u32, &'static str) {
        match self {
            Self::InternalError =>
                (500, 10001, "We encountered an internal error. Please try again."),
            Self::NoSuchKey => (404, 10007, "The specified key does not exist."),
            Self::EntityTooLarge =>
                (400, 100_100, "Your proposed upload exceeds the maximum allowed object size."),
            Self::EntityTooSmall => (400, 10011,
                "Your proposed upload is smaller than the minimum allowed object size."),
            Self::MetadataTooLarge =>
                (400, 10012, "Your metadata headers exceed the maximum allowed metadata size."),
            Self::InvalidObjectName => (400, 10020, "The specified object name is not valid."),
            Self::InvalidMaxKeys =>
                (400, 10022, "MaxKeys params must be positive integer <= 1000."),
            Self::NoSuchUpload => (400, 10024, "The specified multipart upload does not exist."),
            Self::InvalidPart =>
                (400, 10025, "One or more of the specified parts could not be found."),
            Self::InvalidMetadata => (400, 10029, "Metadata missing or invalid"),
            Self::PreconditionFailed =>
                (412, 10031, "At least one of the pre-conditions you specified did not hold."),
            Self::BadDigest => (400, 10037, ""),
            Self::InvalidRange => (416, 10039, "The requested range is not satisfiable"),
            Self::BadUpload => (500, 10048, "There was a problem with the multipart upload."),
        }
    }

    /// The R2 error a store's refusal is, by its S3 code.
    fn of_store(code: &str) -> Option<Self> {
        Some(match code {
            "NoSuchKey" => Self::NoSuchKey,
            "EntityTooLarge" => Self::EntityTooLarge,
            "EntityTooSmall" => Self::EntityTooSmall,
            "MetadataTooLarge" => Self::MetadataTooLarge,
            "NoSuchUpload" => Self::NoSuchUpload,
            "InvalidPart" => Self::InvalidPart,
            "PreconditionFailed" => Self::PreconditionFailed,
            "InvalidRange" => Self::InvalidRange,
            "BadDigest" => Self::BadDigest,
            _ => return None,
        })
    }
}

/// Why a request failed: an R2 error, with the metadata a failed get's
/// precondition answers, or the request's own error, which fails it.
enum Fail {
    R2(Code, String, Option<Value>),
    Kj(crate::Error),
}

impl Fail {
    const fn code(&self) -> Option<Code> {
        match self {
            Self::R2(code, ..) => Some(*code),
            Self::Kj(_) => None,
        }
    }
}

impl Code {
    /// This error, answering `object`'s metadata.
    fn with(self, object: Value) -> Fail {
        Fail::R2(self, self.info().2.to_owned(), Some(object))
    }
}

impl From<Code> for Fail {
    fn from(code: Code) -> Self {
        Self::R2(code, code.info().2.to_owned(), None)
    }
}

impl From<cluster::Error> for Fail {
    fn from(error: cluster::Error) -> Self {
        if let cluster::Error::Refused { code, .. } = &error
            && let Some(code) = Code::of_store(code)
        {
            return code.into();
        }
        tracing::error!("R2: {error}");
        Code::InternalError.into()
    }
}

impl From<crate::Error> for Fail {
    fn from(error: crate::Error) -> Self {
        Self::Kj(error)
    }
}

type Answer<T> = std::result::Result<T, Fail>;

/// What a request answers: its metadata, then a body of this length.
type Reply = (Option<Value>, Option<(u64, Body)>);

const fn meta(value: Value) -> Reply {
    (Some(value), None)
}

pub(super) async fn request(
    shared: &Shared,
    method: Method,
    headers: HeadersRef<'_>,
    mut body: Pin<&mut AsyncInputStream>,
    response: ServiceResponse<'_>,
) -> Result<()> {
    let mut map = http::HeaderMap::new();
    kj_hyper::Head::new(headers.as_ffi()).apply(&mut map, &mut http::Extensions::new());
    let r2 = R2(shared);
    let answer = match method {
        Method::GET => r2.read(&map).await,
        Method::PUT => {
            let answer = r2.write(&map, body.as_mut()).await;
            // The binding sends the whole body before it reads the answer.
            let mut rest = vec![0; 64 << 10];
            while body.as_mut().try_read(&mut rest, 1).await? > 0 {}
            answer
        }
        _ => Err(Code::InternalError.into()),
    };
    let (status, error, (meta, body)) = match answer {
        Ok(reply) => (200, None, reply),
        Err(Fail::Kj(error)) => return Err(error),
        Err(Fail::R2(code, message, object)) => {
            let (status, v4code, _) = code.info();
            let error = json!({ "version": 1, "v4code": v4code, "message": message });
            (status, Some(error), (object, None))
        }
    };
    let mut map = http::HeaderMap::new();
    if let Some(error) = error {
        map.insert("cf-r2-error", header_value(error.to_string())?);
    }
    let meta = meta.map_or_else(Bytes::new, |m| Bytes::from(m.to_string()));
    if !meta.is_empty() {
        map.insert("cf-r2-metadata-size", header_value(meta.len().to_string())?);
    }
    let length = meta.len() as u64 + body.as_ref().map_or(0, |(length, _)| *length);
    // Set explicitly, as the `disk` service does, so that a worker calling
    // in-process sees it too.
    map.insert("content-length", header_value(length.to_string())?);
    let headers = kj_headers(header_table(&shared.factory), &map)?;
    let reason = if status == 200 { "OK" } else { "Error" };
    let headers = HeadersRef::from(&*headers);
    let mut out = response.send(status.into(), reason, headers, Some(length))?;
    out.write(&meta).await?;
    let Some((_, mut body)) = body else {
        return Ok(());
    };
    while let Some(chunk) = body.next().await {
        out.write(&chunk.map_err(|e| kj::disconnected!("{e}"))?)
            .await?;
    }
    Ok(())
}

/// A request's key, refused as R2 refuses it.
fn key(value: &Value) -> Answer<&str> {
    let key = value.as_str().ok_or(Code::InvalidMetadata)?;
    // A `.` or `..` segment could climb out of the bucket's directory in a
    // store or proxy that resolves them.
    if key.len() > MAX_KEY || key.split('/').any(|s| s == "." || s == "..") {
        return Err(Code::InvalidObjectName.into());
    }
    Ok(key)
}

/// A `UInt64` field, `None` if unset: capnp's JSON writes one as a number or
/// a string, and unset as all ones.
fn u64(value: &Value) -> Option<u64> {
    let n = value.as_u64().or_else(|| value.as_str()?.parse().ok())?;
    (n != u64::MAX).then_some(n)
}

fn some_str<'a>(value: &'a Value, name: &str) -> Option<&'a str> {
    value[name].as_str().filter(|s| !s.is_empty())
}

fn only_if(req: &Value) -> Option<&Value> {
    req.get("onlyIf").filter(|c| c.is_object())
}

fn header_safe(value: &str) -> bool {
    value
        .bytes()
        .all(|b| b == b'\t' || (b' '..=b'~').contains(&b))
}

/// The R2 record stored with an object.
#[derive(Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
struct Record {
    custom: BTreeMap<String, String>,
    /// The httpFields not stored as headers.
    http: Map<String, Value>,
    /// Hex, by algorithm, but MD5, which is the etag.
    checksums: BTreeMap<String, String>,
    storage_class: String,
    /// Our time of a put, in ms; a multipart upload's is the store's.
    uploaded: Option<u64>,
}

/// The record, content headers and user metadata a put or a multipart
/// upload stores, refused if its customMetadata is too large.
fn record(req: &Value) -> Answer<(Record, Content)> {
    let fields = &req["httpFields"];
    let mut record = Record {
        storage_class: some_str(req, "storageClass")
            .unwrap_or("Standard")
            .to_owned(),
        ..Record::default()
    };
    for (field, _) in FIELDS {
        if let Some(v) = some_str(fields, field).filter(|v| !header_safe(v)) {
            record.http.insert(field.to_owned(), v.into());
        }
    }
    if let Some(expiry) = u64(&fields["cacheExpiry"]) {
        record.http.insert("cacheExpiry".to_owned(), expiry.into());
    }
    let content = Content::from_fn(|name| {
        let (field, _) = FIELDS.iter().find(|(_, header)| *header == name)?;
        some_str(fields, field)
            .filter(|v| header_safe(v))
            .map(str::to_owned)
    });
    let custom = req["customFields"]
        .as_array()
        .map_or(&[][..], Vec::as_slice);
    for kv in custom {
        let (k, v) = (kv["k"].as_str(), kv["v"].as_str());
        let (k, v) = k.zip(v).ok_or(Code::InvalidMetadata)?;
        record.custom.insert(k.to_owned(), v.to_owned());
    }
    // As R2 counts it.
    let length =
        |s: &str| s.encode_utf16().count() << usize::from(s.chars().any(|c| c >= '\u{100}'));
    let size: usize = record
        .custom
        .iter()
        .map(|(k, v)| length(k) + length(v))
        .sum();
    if size > MAX_METADATA {
        return Err(Code::MetadataTooLarge.into());
    }
    Ok((record, content))
}

/// The user metadata that stores `record`: its JSON, escaped to ASCII.
fn envelope(record: &Record) -> Vec<(String, String)> {
    let json = serde_json::to_string(record).unwrap_or_default();
    let mut ascii = String::with_capacity(json.len());
    for c in json.chars() {
        if c < '\u{7f}' {
            ascii.push(c);
        } else {
            c.encode_utf16(&mut [0; 2])
                .iter()
                .for_each(|u| ascii += &format!("\\u{u:04x}"));
        }
    }
    vec![(ENVELOPE.to_owned(), ascii)]
}

/// An object as R2 describes it.
struct Item {
    key: String,
    size: u64,
    etag: String,
    version: Version,
    uploaded: u64,
    http: Map<String, Value>,
    record: Record,
}

impl Item {
    fn of(key: &str, o: &Object) -> Self {
        let found = o.metadata.iter().find(|(name, _)| name == ENVELOPE);
        let record = found.and_then(|(_, json)| serde_json::from_str::<Record>(json).ok());
        let record = record.unwrap_or_else(|| Record {
            custom: o.metadata.iter().cloned().collect(),
            storage_class: "Standard".to_owned(),
            ..Record::default()
        });
        let mut http = Map::new();
        for ((field, _), (_, value)) in FIELDS.iter().zip(o.headers.named()) {
            if let Some(value) = value {
                http.insert((*field).to_owned(), value.into());
            }
        }
        http.extend(record.http.clone());
        Self {
            key: key.to_owned(),
            size: o.size,
            etag: o.etag.clone(),
            version: o.version.clone(),
            uploaded: record.uploaded.unwrap_or(o.modified_ms),
            http,
            record,
        }
    }

    /// As the binding's `R2HeadResponse`; with `include`, its `httpFields`
    /// and `customFields`.
    fn json(&self, range: Option<(u64, u64)>, include: (bool, bool)) -> Value {
        let mut checksums = Map::new();
        if self.etag.len() == 32 && self.etag.bytes().all(|b| b.is_ascii_hexdigit()) {
            checksums.insert("0".to_owned(), self.etag.clone().into());
        }
        for (i, (name, _)) in ALGORITHMS.iter().enumerate().skip(1) {
            if let Some(sum) = self.record.checksums.get(*name) {
                checksums.insert(i.to_string(), sum.clone().into());
            }
        }
        let custom = self
            .record
            .custom
            .iter()
            .map(|(k, v)| json!({ "k": k, "v": v }));
        let mut json = json!({
            "name": self.key,
            // The store's version names the bytes this key holds now; R2's
            // is 32 hex digits.
            "version": hex(&md5(self.version.as_str().as_bytes())),
            "size": self.size,
            "etag": self.etag,
            "uploaded": self.uploaded,
            "httpFields": if include.0 { self.http.clone() } else { Map::new() },
            "customFields": if include.1 { custom.collect() } else { vec![] },
            "checksums": checksums,
            "storageClass": self.record.storage_class,
        });
        if let Some((offset, length)) = range {
            json["range"] = json!({ "offset": offset, "length": length });
        }
        json
    }
}

const ALL: (bool, bool) = (true, true);

/// The hashes of a put's value as it is read, for the checksums it names.
struct Hashes(Vec<(usize, Vec<u8>, Hash)>);

enum Hash {
    Md5(Md5),
    Sha(ring::digest::Context),
}

impl Hashes {
    fn of(req: &Value) -> Answer<Self> {
        use ring::digest;
        let mut hashes = Vec::new();
        for (i, (field, _)) in ALGORITHMS.iter().enumerate() {
            let Some(given) = some_str(req, field) else {
                continue;
            };
            let given = if i == 0 {
                use base64::Engine;
                base64::engine::general_purpose::STANDARD.decode(given).ok()
            } else {
                cluster::unhex(&given.to_ascii_lowercase())
            };
            let algorithm = [
                &digest::SHA1_FOR_LEGACY_USE_ONLY,
                &digest::SHA256,
                &digest::SHA384,
                &digest::SHA512,
            ];
            let hash = match i {
                0 => Hash::Md5(Md5::default()),
                i => Hash::Sha(digest::Context::new(algorithm[i - 1])),
            };
            hashes.push((i, given.ok_or(Code::InvalidMetadata)?, hash));
        }
        Ok(Self(hashes))
    }

    fn update(&mut self, data: &[u8]) {
        for (_, _, hash) in &mut self.0 {
            match hash {
                Hash::Md5(md5) => md5.update(data),
                Hash::Sha(sha) => sha.update(data),
            }
        }
    }

    /// The checksums as the record keeps them: hex, but MD5.
    fn record(&self) -> BTreeMap<String, String> {
        let sha = self.0.iter().filter(|(i, ..)| *i > 0);
        sha.map(|(i, given, _)| (ALGORITHMS[*i].0.to_owned(), hex(given)))
            .collect()
    }

    fn check(self) -> Answer<()> {
        for (i, given, hash) in self.0 {
            let got = match hash {
                Hash::Md5(md5) => md5.finish().to_vec(),
                Hash::Sha(sha) => sha.finish().as_ref().to_vec(),
            };
            if got != given {
                let name = ALGORITHMS[i].1;
                let message = format!(
                    "The {name} checksum you specified did not match what we received.\n\
                     You provided a {name} checksum with value: {}\n\
                     Actual {name} was: {}",
                    hex(&given),
                    hex(&got),
                );
                return Err(Fail::R2(Code::BadDigest, message, None));
            }
        }
        Ok(())
    }
}

/// Fills `buffer` from `body` up to `limit` bytes: whether it is full. It
/// grows as it is read, so a small body costs a small buffer.
async fn fill(
    body: &mut Pin<&mut AsyncInputStream>,
    buffer: &mut Vec<u8>,
    limit: usize,
) -> Result<bool> {
    while buffer.len() < limit {
        let len = buffer.len();
        buffer.resize(limit.min(len.max(64 << 10) * 2), 0);
        let read = body.as_mut().try_read(&mut buffer[len..], 1).await?;
        buffer.truncate(len + read);
        if read == 0 {
            return Ok(false);
        }
    }
    Ok(true)
}

/// Reads the next part of `body` into `part`, which holds what was read of
/// it already, then one byte past it into `next`, which stays empty at the
/// end of the body.
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

/// Whether an object passes an `R2Conditional`, as R2's gateway decides
/// (RFC 7232, section 6), after Miniflare's `_testR2Conditional`.
fn passes(cond: &Value, o: Option<&Item>) -> bool {
    let list = |name| cond[name].as_array().filter(|l| !l.is_empty());
    let (matches, none_match) = (list("etagMatches"), list("etagDoesNotMatch"));
    let (before, after) = (u64(&cond["uploadedBefore"]), u64(&cond["uploadedAfter"]));
    let Some(o) = o else {
        return matches.is_none() && after.is_none();
    };
    let has = |list: &Vec<Value>, weak| {
        list.iter().any(|c| {
            c["type"] == "wildcard"
                || (c["value"] == o.etag.as_str() && (c["type"] == "strong" || weak))
        })
    };
    let t = |ms: u64| {
        if cond["secondsGranularity"] == true {
            ms / 1000 * 1000
        } else {
            ms
        }
    };
    let if_match = matches.is_none_or(|l| has(l, false));
    let if_none_match = none_match.is_none_or(|l| !has(l, true));
    let uploaded = t(o.uploaded);
    let modified_since =
        after.is_none_or(|a| t(a) < uploaded) || (none_match.is_some() && if_none_match);
    let unmodified_since =
        before.is_none_or(|b| uploaded < t(b)) || (matches.is_some() && if_match);
    if_match && if_none_match && modified_since && unmodified_since
}

/// A Range header as Miniflare reads it: other than one range is all of
/// the object, `None`.
fn header_range(header: &str) -> Option<Range> {
    let spec = header.trim();
    let unit = spec.get(..5).filter(|u| u.eq_ignore_ascii_case("bytes"))?;
    let spec = spec[unit.len()..].trim_start().strip_prefix('=')?;
    let (first, last) = spec.split_once('-')?;
    let number = |s: &str| {
        let s = s.trim();
        (s.is_empty() || s.bytes().all(|b| b.is_ascii_digit())).then_some(s.parse::<u64>().ok())
    };
    match (number(first)?, number(last)?) {
        (None, Some(n)) if n > 0 => Some(Range::Suffix(n)),
        (Some(offset), None) => Some(Range::From {
            offset,
            length: None,
        }),
        (Some(first), Some(last)) if first <= last => Some(Range::From {
            offset: first,
            length: Some((last - first).saturating_add(1)),
        }),
        _ => None,
    }
}

/// The range a get asks for; `header` reads its Range header, if any.
fn range(req: &Value, header: bool) -> Answer<Option<Range>> {
    if let Some(h) = some_str(req, "rangeHeader").filter(|_| header) {
        return Ok(header_range(h));
    }
    let r = &req["range"];
    let (offset, length) = (u64(&r["offset"]), u64(&r["length"]));
    Ok(match (u64(&r["suffix"]), offset, length) {
        (Some(0), ..) | (_, _, Some(0)) => return Err(Code::InvalidRange.into()),
        (Some(n), ..) => Some(Range::Suffix(n)),
        (None, None, None) => None,
        (None, offset, length) => Some(Range::From {
            offset: offset.unwrap_or(0),
            length,
        }),
    })
}

struct R2<'a>(&'a Shared);

impl R2<'_> {
    async fn head(&self, key: &str) -> Answer<Option<Item>> {
        let object = self.0.bucket.head(key).await?;
        Ok(object.map(|o| Item::of(key, &o)))
    }

    async fn read(&self, headers: &http::HeaderMap) -> Answer<Reply> {
        let req = headers.get("cf-r2-request");
        let req = req.and_then(|v| serde_json::from_slice::<Value>(v.as_bytes()).ok());
        let req = req.ok_or(Code::InvalidMetadata)?;
        match req["method"].as_str() {
            Some("head") => {
                let o = self.head(key(&req["object"])?).await?;
                let o = o.ok_or(Code::NoSuchKey)?;
                Ok(meta(o.json(Some((0, o.size)), ALL)))
            }
            Some("get") => self.get(&req).await,
            Some("list") => self.list(&req).await,
            _ => Err(Code::InternalError.into()),
        }
    }

    async fn get(&self, req: &Value) -> Answer<Reply> {
        let key = key(&req["object"])?;
        let mut header = some_str(req, "rangeHeader").is_some();
        loop {
            let checked = if let Some(cond) = only_if(req) {
                let o = self.head(key).await?.ok_or(Code::NoSuchKey)?;
                if !passes(cond, Some(&o)) {
                    let meta = o.json(Some((0, o.size)), ALL);
                    return Err(Code::PreconditionFailed.with(meta));
                }
                Some(o.version)
            } else {
                None
            };
            let read = match self.0.bucket.read(key, range(req, header)?).await {
                // Past the end, as Miniflare reads it: a Range header's is
                // all of the value, and a range starting at its end is none.
                Err(cluster::Error::Refused { status: 416, .. }) if header => {
                    header = false;
                    continue;
                }
                Err(cluster::Error::Refused { status: 416, .. }) => {
                    let o = self.head(key).await?.ok_or(Code::NoSuchKey)?;
                    if u64(&req["range"]["offset"]).unwrap_or(0) != o.size {
                        return Err(Code::InvalidRange.into());
                    }
                    return Ok(meta(o.json(Some((o.size, 0)), ALL)));
                }
                read => read?,
            };
            let (object, body) = read.ok_or(Code::NoSuchKey)?;
            // The precondition held for the version it was checked against.
            if checked.is_some_and(|v| v != object.version) {
                continue;
            }
            let (offset, length) = object.range.unwrap_or((0, object.size));
            let meta = Item::of(key, &object).json(Some((offset, length)), ALL);
            return Ok((Some(meta), Some((length, body))));
        }
    }

    async fn list(&self, req: &Value) -> Answer<Reply> {
        let limit = match req["limit"].as_u64() {
            None | Some(0xffff_ffff) => MAX_LIST,
            Some(n) => n,
        };
        if !(1..=MAX_LIST).contains(&limit) {
            return Err(Code::InvalidMaxKeys.into());
        }
        // A cursor is the store's token or the key a short reply stopped
        // after; another lists nothing, as in Miniflare.
        let start_after = some_str(req, "startAfter");
        let (token, start_after) = match some_str(req, "cursor") {
            None => (None, start_after),
            Some(c) if c.starts_with(CURSOR) => (Some(&c[CURSOR.len()..]), start_after),
            Some(c) if c.starts_with(AFTER) => (None, Some(&c[AFTER.len()..])),
            Some(_) => {
                let empty = json!({ "objects": [], "truncated": false, "delimitedPrefixes": [] });
                return Ok(meta(empty));
            }
        };
        let include = req["include"].as_array().map_or(&[][..], Vec::as_slice);
        let include = (include.contains(&0.into()), include.contains(&1.into()));
        let page = self.0.bucket.page(
            some_str(req, "prefix").unwrap_or(""),
            start_after,
            some_str(req, "delimiter"),
            limit as usize,
            token,
        );
        let page = page.await?;
        // Metadata the listing doesn't carry is a HEAD of each object.
        let objects = futures::stream::iter(page.entries).map(|e| async move {
            let o = Object {
                size: e.size,
                version: e.version,
                etag: e.etag,
                modified_ms: e.modified_ms,
                headers: Content::default(),
                metadata: vec![],
                range: None,
            };
            let mut item = Item::of(&e.key, &o);
            if include != (false, false) {
                item = self.head(&e.key).await?.unwrap_or(item);
            }
            Ok::<_, Fail>(item.json(None, include))
        });
        let objects: Vec<Answer<Value>> = objects.buffered(PARALLEL).collect().await;
        let mut objects = objects.into_iter().collect::<Answer<Vec<_>>>()?;
        let mut cursor = page.token.map(|token| format!("{CURSOR}{token}"));
        let mut prefixes = page.prefixes;
        // Fewer than `limit` when the reply would be too large, as R2 does.
        let mut size = 0;
        let fit = objects.iter().take_while(|o| {
            size += o.to_string().len();
            size <= MAX_REPLY
        });
        let fit = fit.count().max(1);
        if fit < objects.len() {
            objects.truncate(fit);
            let last = objects[fit - 1]["name"]
                .as_str()
                .unwrap_or_default()
                .to_owned();
            prefixes.retain(|p| *p < last);
            cursor = Some(format!("{AFTER}{last}"));
        }
        let mut listed = json!({
            "objects": objects,
            "truncated": cursor.is_some(),
            "delimitedPrefixes": prefixes,
        });
        if let Some(cursor) = cursor {
            listed["cursor"] = cursor.into();
        }
        Ok(meta(listed))
    }

    async fn write(
        &self,
        headers: &http::HeaderMap,
        mut body: Pin<&mut AsyncInputStream>,
    ) -> Answer<Reply> {
        let number = |name| headers.get(name)?.to_str().ok()?.parse::<u64>().ok();
        let n = number("cf-r2-metadata-size").ok_or(Code::InvalidMetadata)?;
        let length = number("content-length").or_else(|| body.as_mut().try_get_length());
        let mut prefix = Vec::new();
        fill(&mut body, &mut prefix, n as usize).await?;
        let req: Value = serde_json::from_slice(&prefix).map_err(|_| Code::InvalidMetadata)?;
        let size = || {
            let message = "Content-Length is missing or invalid".to_owned();
            length.and_then(|l| l.checked_sub(n)).ok_or(Fail::R2(
                Code::InvalidMetadata,
                message,
                None,
            ))
        };
        let bucket = &self.0.bucket;
        match req["method"].as_str() {
            Some("put") => self.put(&req, body, size()?).await,
            Some("delete") => {
                let keys = match req["objects"].as_array() {
                    Some(keys) => keys.iter().map(key).collect::<Answer<Vec<_>>>()?,
                    None => vec![key(&req["object"])?],
                };
                let deleted = futures::stream::iter(keys).map(|k| bucket.delete(k));
                let deleted: Vec<_> = deleted.buffered(PARALLEL).collect().await;
                deleted.into_iter().collect::<cluster::Result<Vec<_>>>()?;
                Ok((None, None))
            }
            Some("createMultipartUpload") => {
                let (record, content) = record(&req)?;
                let k = key(&req["object"])?;
                let id = bucket.begin(k, &content, &envelope(&record)).await?;
                Ok(meta(json!({ "uploadId": id })))
            }
            Some("uploadPart") => {
                let (k, id) = (key(&req["object"])?, upload(&req)?);
                let n = req["partNumber"]
                    .as_u64()
                    .and_then(|n| u32::try_from(n).ok());
                let n = n.ok_or(Code::InvalidMetadata)?;
                if size()? > MAX_PART as u64 {
                    return Err(Code::EntityTooLarge.into());
                }
                let mut part = Vec::new();
                fill(&mut body, &mut part, MAX_PART).await?;
                let etag = bucket.part(k, id, n, part.into()).await?;
                Ok(meta(json!({ "etag": etag })))
            }
            Some("completeMultipartUpload") => self.complete(&req).await,
            Some("abortMultipartUpload") => {
                let aborted = bucket.abort(key(&req["object"])?, upload(&req)?).await;
                match aborted.map_err(Fail::from) {
                    Err(f) if f.code() != Some(Code::NoSuchUpload) => Err(f),
                    _ => Ok((None, None)),
                }
            }
            _ => Err(Code::InternalError.into()),
        }
    }

    async fn put(
        &self,
        req: &Value,
        mut body: Pin<&mut AsyncInputStream>,
        size: u64,
    ) -> Answer<Reply> {
        let key = key(&req["object"])?;
        if size > MAX_VALUE {
            return Err(Code::EntityTooLarge.into());
        }
        let mut hashes = Hashes::of(req)?;
        let (mut record, content) = record(req)?;
        record.checksums = hashes.record();
        // The value is not read for a precondition that fails already. The
        // store fences a write of one part on the version it was checked
        // against.
        let guard = if let Some(cond) = only_if(req) {
            let o = self.head(key).await?;
            if !passes(cond, o.as_ref()) {
                return Err(Code::PreconditionFailed.into());
            }
            Some(o.map_or(Guard::Absent, |o| Guard::Match(o.version)))
        } else {
            None
        };
        let bucket = &self.0.bucket;
        let (mut part, mut next) = (Vec::new(), Vec::new());
        next_part(&mut body, &mut part, &mut next).await?;
        hashes.update(&part);
        if next.is_empty() {
            hashes.check()?;
            record.uploaded = Some(cluster::now_ms());
            let metadata = envelope(&record);
            let size = part.len() as u64;
            let body = Payload::Bytes(part.into());
            let written = bucket.write(key, body, guard, &content, &metadata).await?;
            let written = written.ok_or(Code::PreconditionFailed)?;
            let o = Object {
                size,
                version: written.version.unwrap_or_default(),
                etag: written.etag,
                modified_ms: 0,
                headers: content,
                metadata,
                range: None,
            };
            return Ok(meta(Item::of(key, &o).json(None, ALL)));
        }
        let id = bucket.begin(key, &content, &envelope(&record)).await?;
        let uploaded = async {
            let mut parts = Vec::new();
            for n in 1.. {
                parts.push((n, bucket.part(key, &id, n, Bytes::from(part)).await?));
                if next.is_empty() {
                    break;
                }
                part = std::mem::take(&mut next);
                next_part(&mut body, &mut part, &mut next).await?;
                hashes.update(&part);
            }
            hashes.check()?;
            bucket.complete(key, &id, &parts).await?;
            Ok::<_, Fail>(())
        };
        if let Err(e) = uploaded.await {
            let _ = bucket.abort(key, &id).await;
            return Err(e);
        }
        self.described(key).await
    }

    /// The object `key` was just written as.
    async fn described(&self, key: &str) -> Answer<Reply> {
        let o = self.head(key).await?.ok_or(Code::InternalError)?;
        Ok(meta(o.json(None, ALL)))
    }

    /// Completes an upload with the chosen parts, checked as R2 checks
    /// them: unknown parts, too small ones in the order given, then parts
    /// but the last of unequal size.
    async fn complete(&self, req: &Value) -> Answer<Reply> {
        let (key, id) = (key(&req["object"])?, upload(req)?);
        let chosen = req["parts"].as_array().map_or(&[][..], Vec::as_slice);
        let chosen = chosen.iter().map(|p| {
            let n = p["part"].as_u64().and_then(|n| u32::try_from(n).ok());
            Some((n?, p["etag"].as_str()?))
        });
        let chosen = chosen
            .collect::<Option<Vec<_>>>()
            .ok_or(Code::InvalidMetadata)?;
        let mut numbers: Vec<_> = chosen.iter().map(|(n, _)| n).collect();
        numbers.sort_unstable();
        numbers.dedup();
        if numbers.len() != chosen.len() {
            return Err(Code::InternalError.into());
        }
        let bucket = &self.0.bucket;
        let uploaded = match bucket.parts(key, id).await.map_err(Fail::from) {
            // An upload this key never had, as Miniflare answers it.
            Err(f) if f.code() == Some(Code::NoSuchUpload) && chosen.is_empty() => {
                return Err(Code::InternalError.into());
            }
            uploaded => uploaded?,
        };
        let uploaded: std::collections::HashMap<_, _> =
            uploaded.iter().map(|p| ((p.0, &p.1[..]), p)).collect();
        let picked = chosen
            .iter()
            .map(|&(n, etag)| uploaded.get(&(n, etag)).copied().ok_or(Code::InvalidPart));
        let mut picked = picked.collect::<std::result::Result<Vec<_>, _>>()?;
        let but_last = &picked[..picked.len().saturating_sub(1)];
        if but_last.iter().any(|(.., size)| *size < self.0.min_part) {
            return Err(Code::EntityTooSmall.into());
        }
        picked.sort_by_key(|(n, ..)| *n);
        if let Some((_, _, first)) = picked.first() {
            let (but_last, last) = (&picked[..picked.len() - 1], picked[picked.len() - 1]);
            if but_last.iter().any(|(.., s)| s != first) || last.2 > *first {
                return Err(Code::BadUpload.into());
            }
            let parts: Vec<_> = picked
                .iter()
                .map(|(n, etag, _)| (*n, etag.clone()))
                .collect();
            bucket.complete(key, id, &parts).await?;
        } else {
            // A store can't complete an upload of no parts.
            let empty = Payload::Bytes(Bytes::new());
            bucket
                .write(key, empty, None, &Content::default(), &[])
                .await?;
            let _ = bucket.abort(key, id).await;
        }
        self.described(key).await
    }
}

fn upload(req: &Value) -> Answer<&str> {
    Ok(req["uploadId"].as_str().ok_or(Code::InvalidMetadata)?)
}
