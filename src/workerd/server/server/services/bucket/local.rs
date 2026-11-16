// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The store of a `bucket` service without a cluster: a directory on local
//! disk, with S3's semantics.
//!
//! - `objects/<md5 of key>`: an object's key, size, `ETag`, version, time,
//!   content headers, user metadata and blob, as JSON. Named by digest: a
//!   key may be longer than a file name, and S3 holds `a` and `a/b` at
//!   once, which a file tree cannot. All are read into an index when the
//!   store opens, so a listing reads no files.
//! - `uploads/<id>`: a multipart upload's key, headers, metadata and parts.
//! - `blobs/<id>`: an object's or a part's bytes. A blob is never rewritten:
//!   a write takes a new one and removes the old.
//!
//! Every file is written to `tmp/` first and renamed in, so a reader never
//! sees a partial one. A version is random, so a guard naming an object's
//! version fails once it is rewritten, even with the same bytes. Disk I/O
//! is synchronous, as in the `disk` service, so each call is atomic on the
//! loop thread; one process serves a directory.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::io::Read;
use std::io::Seek;
use std::io::Write;
use std::ops::Bound;
use std::path::Path;
use std::path::PathBuf;

use bytes::Bytes;
use cluster::Body;
use cluster::Entry;
use cluster::Error;
use cluster::Guard;
use cluster::Headers;
use cluster::Object;
use cluster::Page;
use cluster::Payload;
use cluster::Range;
use cluster::Result;
use cluster::Store;
use cluster::Version;
use cluster::Written;
use cluster::hex;
use cluster::md5;
use cluster::refused;
use futures::StreamExt;
use serde_json::Value as Json;
use serde_json::json;

/// The chunks a read streams a body in.
const CHUNK: usize = 64 << 10;

/// An object, as its file in `objects/` holds it.
#[derive(Clone)]
struct Meta {
    key: String,
    object: Object,
    blob: String,
}

/// A part of an upload: its `ETag`, size and blob.
type Part = (String, u64, String);

/// A multipart upload, as its file in `uploads/` holds it.
struct Upload {
    key: String,
    headers: Headers,
    metadata: Vec<(String, String)>,
    parts: BTreeMap<u32, Part>,
}

pub struct Local {
    root: PathBuf,
    /// The objects, by key.
    index: RefCell<BTreeMap<String, Meta>>,
    /// The least size of a part but the last, at completion.
    min_part: u64,
}

fn random() -> String {
    hex(&rand::random::<[u8; 16]>())
}

fn headers_json(headers: &Headers) -> Json {
    let set = headers.named().into_iter();
    Json::Object(
        set.filter_map(|(n, v)| Some((n.to_owned(), Json::from(v?))))
            .collect(),
    )
}

fn headers_of(json: &Json) -> Headers {
    Headers::from_fn(|name| json.get(name)?.as_str().map(str::to_owned))
}

fn metadata_of(json: &Json) -> Vec<(String, String)> {
    serde_json::from_value(json.clone()).unwrap_or_default()
}

fn text(json: &Json, name: &str) -> String {
    json[name].as_str().unwrap_or_default().to_owned()
}

fn bytes(body: Payload) -> Result<Bytes> {
    Ok(match body {
        Payload::Bytes(bytes) => bytes,
        Payload::File(path) => std::fs::read(path)?.into(),
    })
}

impl Meta {
    fn to_json(&self) -> Json {
        let o = &self.object;
        json!({
            "key": self.key, "size": o.size, "etag": o.etag, "version": o.version.as_str(),
            "modified": o.modified_ms, "headers": headers_json(&o.headers),
            "metadata": o.metadata, "blob": self.blob,
        })
    }

    fn of(json: &Json) -> Option<Self> {
        Some(Self {
            key: json["key"].as_str()?.to_owned(),
            object: Object {
                size: json["size"].as_u64()?,
                version: Version::new(text(json, "version")),
                etag: text(json, "etag"),
                modified_ms: json["modified"].as_u64()?,
                headers: headers_of(&json["headers"]),
                metadata: metadata_of(&json["metadata"]),
                range: None,
            },
            blob: text(json, "blob"),
        })
    }
}

impl Upload {
    fn to_json(&self) -> Json {
        let parts: BTreeMap<_, _> = self.parts.iter().map(|(n, p)| (n.to_string(), p)).collect();
        json!({
            "key": self.key, "headers": headers_json(&self.headers),
            "metadata": self.metadata, "parts": parts,
        })
    }

    fn of(json: &Json) -> Option<Self> {
        let parts: BTreeMap<String, Part> = serde_json::from_value(json["parts"].clone()).ok()?;
        Some(Self {
            key: json["key"].as_str()?.to_owned(),
            headers: headers_of(&json["headers"]),
            metadata: metadata_of(&json["metadata"]),
            parts: parts
                .into_iter()
                .filter_map(|(n, p)| Some((n.parse().ok()?, p)))
                .collect(),
        })
    }
}

impl Local {
    /// The store in the directory `root`, made if missing.
    pub fn open(root: &Path, min_part: u64) -> Result<Self> {
        for dir in ["objects", "uploads", "blobs", "tmp"] {
            std::fs::create_dir_all(root.join(dir))?;
        }
        let mut index = BTreeMap::new();
        for file in std::fs::read_dir(root.join("objects"))? {
            let json = serde_json::from_slice(&std::fs::read(file?.path())?);
            if let Some(meta) = json.ok().as_ref().and_then(Meta::of) {
                index.insert(meta.key.clone(), meta);
            }
        }
        Ok(Self {
            root: root.to_owned(),
            index: RefCell::new(index),
            min_part,
        })
    }

    fn path(&self, dir: &str, name: &str) -> PathBuf {
        self.root.join(dir).join(name)
    }

    /// Writes `body` to `dir/name`, by way of `tmp/`.
    fn save(&self, dir: &str, name: &str, body: &[u8]) -> Result<()> {
        let temp = self.path("tmp", &random());
        std::fs::write(&temp, body)?;
        std::fs::rename(&temp, self.path(dir, name))?;
        Ok(())
    }

    /// Writes `body` to a new blob, answering its ID.
    fn put_blob(&self, body: &[u8]) -> Result<String> {
        let id = random();
        self.save("blobs", &id, body)?;
        Ok(id)
    }

    fn drop_blob(&self, id: &str) {
        let _ = std::fs::remove_file(self.path("blobs", id));
    }

    fn object_file(key: &str) -> String {
        hex(&md5(key.as_bytes()))
    }

    /// Writes the blob `blob` at `key` if `guard` holds; otherwise drops it.
    fn store(
        &self,
        key: &str,
        mut object: Object,
        blob: String,
        guard: Option<&Guard>,
    ) -> Result<Option<Object>> {
        let current = self.index.borrow().get(key).cloned();
        let holds = match (guard, &current) {
            (None, _) | (Some(Guard::Absent), None) => true,
            (Some(Guard::Match(version)), Some(meta)) => *version == meta.object.version,
            _ => false,
        };
        if !holds {
            self.drop_blob(&blob);
            return Ok(None);
        }
        object.version = Version::new(random());
        object.modified_ms = cluster::now_ms();
        let meta = Meta {
            key: key.to_owned(),
            object: object.clone(),
            blob,
        };
        self.save(
            "objects",
            &Self::object_file(key),
            meta.to_json().to_string().as_bytes(),
        )?;
        self.index.borrow_mut().insert(key.to_owned(), meta);
        if let Some(old) = current {
            self.drop_blob(&old.blob);
        }
        Ok(Some(object))
    }

    /// Upload `id` of `key`.
    fn upload(&self, key: &str, id: &str) -> Result<Upload> {
        let json = std::fs::read(self.path("uploads", id)).ok();
        let json: Option<Json> = json.and_then(|j| serde_json::from_slice(&j).ok());
        let upload = json.as_ref().and_then(Upload::of).filter(|u| u.key == key);
        upload.ok_or_else(|| refused(format!("UPLOAD {key}"), 404, "NoSuchUpload"))
    }

    fn save_upload(&self, id: &str, upload: &Upload) -> Result<()> {
        self.save("uploads", id, upload.to_json().to_string().as_bytes())
    }

    /// Drops upload `id` and its parts.
    fn forget(&self, id: &str, upload: &Upload) {
        for (_, _, blob) in upload.parts.values() {
            self.drop_blob(blob);
        }
        let _ = std::fs::remove_file(self.path("uploads", id));
    }
}

/// The chunks of `length` bytes of `file`, from where it is.
fn chunks(file: std::fs::File, length: u64) -> Body {
    let read = move |(mut file, left): (std::fs::File, u64)| async move {
        if left == 0 {
            return None;
        }
        let n = usize::try_from(left).map_or(CHUNK, |n| n.min(CHUNK));
        let mut chunk = vec![0; n];
        Some(match file.read_exact(&mut chunk) {
            Ok(()) => (Ok(Bytes::from(chunk)), (file, left - n as u64)),
            // An error ends the body.
            Err(e) => (Err(Error::Io(e)), (file, 0)),
        })
    };
    futures::stream::unfold((file, length), read).boxed_local()
}

fn new_object(size: u64, etag: String, headers: &Headers, metadata: &[(String, String)]) -> Object {
    Object {
        size,
        version: Version::new(String::new()),
        etag,
        modified_ms: 0,
        headers: headers.clone(),
        metadata: metadata.to_vec(),
        range: None,
    }
}

#[async_trait::async_trait(?Send)]
impl Store for Local {
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
        let Some(meta) = self.index.borrow().get(key).cloned() else {
            return Ok(None);
        };
        let blob = self.path("blobs", &meta.blob);
        let body = match to {
            Some(to) => {
                // Not `fs::copy`: its `copy_file_range` doesn't link against
                // the arm64 Linux sysroot.
                std::fs::write(to, std::fs::read(blob)?)?;
                Vec::new()
            }
            None => std::fs::read(blob)?,
        };
        Ok(Some((body, meta.object.version)))
    }

    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>> {
        let metadata: Vec<_> = metadata
            .iter()
            .map(|(n, v)| ((*n).to_owned(), v.clone()))
            .collect();
        let written = self
            .write(key, body, guard, &Headers::default(), &metadata)
            .await?;
        Ok(written.and_then(|w| w.version))
    }

    async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        Ok(self
            .page(prefix, None, None, usize::MAX, None)
            .await?
            .entries)
    }

    async fn delete(&self, key: &str) -> Result<()> {
        if let Some(meta) = self.index.borrow_mut().remove(key) {
            let _ = std::fs::remove_file(self.path("objects", &Self::object_file(key)));
            self.drop_blob(&meta.blob);
        }
        Ok(())
    }

    async fn head(&self, key: &str) -> Result<Option<Object>> {
        Ok(self.index.borrow().get(key).map(|meta| meta.object.clone()))
    }

    async fn read(&self, key: &str, range: Option<Range>) -> Result<Option<(Object, Body)>> {
        let Some(Meta {
            mut object, blob, ..
        }) = self.index.borrow().get(key).cloned()
        else {
            return Ok(None);
        };
        let (offset, length) = match range {
            None => (0, object.size),
            Some(range) => range
                .of(object.size)
                .ok_or_else(|| refused(format!("GET {key}"), 416, "InvalidRange"))?,
        };
        object.range = range.map(|_| (offset, length));
        // Opened now, so a write that replaces the object meanwhile does not
        // change what this read answers.
        let mut file = std::fs::File::open(self.path("blobs", &blob))?;
        file.seek(std::io::SeekFrom::Start(offset))?;
        Ok(Some((object, chunks(file, length))))
    }

    /// A token is the last key or common prefix of its page.
    async fn page(
        &self,
        prefix: &str,
        start_after: Option<&str>,
        delimiter: Option<&str>,
        max: usize,
        token: Option<&str>,
    ) -> Result<Page> {
        let after = start_after.max(token).unwrap_or("");
        let index = self.index.borrow();
        let from = if after >= prefix && !after.is_empty() {
            Bound::Excluded(after)
        } else {
            Bound::Included(prefix)
        };
        let mut page = Page::default();
        let mut last: Option<String> = None;
        let later = index.range::<str, _>((from, Bound::Unbounded));
        for (key, meta) in later.take_while(|(key, _)| key.starts_with(prefix)) {
            let rest = &key[prefix.len()..];
            let common = delimiter
                .filter(|d| !d.is_empty())
                .and_then(|d| Some(format!("{prefix}{}", &rest[..rest.find(d)? + d.len()])));
            if let Some(common) = &common
                && (common.as_str() <= after || last.as_ref() == Some(common))
            {
                continue;
            }
            if page.entries.len() + page.prefixes.len() == max {
                page.token = last;
                break;
            }
            if let Some(common) = common {
                page.prefixes.push(common.clone());
                last = Some(common);
            } else {
                let o = &meta.object;
                page.entries.push(Entry {
                    key: key.clone(),
                    size: o.size,
                    etag: o.etag.clone(),
                    modified_ms: o.modified_ms,
                });
                last = Some(key.clone());
            }
        }
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
        let body = bytes(body)?;
        let etag = hex(&md5(&body));
        let blob = self.put_blob(&body)?;
        let object = new_object(body.len() as u64, etag, headers, metadata);
        let stored = self.store(key, object, blob, guard.as_ref())?;
        Ok(stored.map(|o| Written {
            version: Some(o.version),
            etag: o.etag,
            modified_ms: Some(o.modified_ms),
        }))
    }

    async fn begin(
        &self,
        key: &str,
        headers: &Headers,
        metadata: &[(String, String)],
    ) -> Result<String> {
        let id = random();
        let upload = Upload {
            key: key.to_owned(),
            headers: headers.clone(),
            metadata: metadata.to_vec(),
            parts: BTreeMap::new(),
        };
        self.save_upload(&id, &upload)?;
        Ok(id)
    }

    async fn part(&self, key: &str, upload: &str, n: u32, body: Bytes) -> Result<String> {
        if !(1..=10_000).contains(&n) {
            return Err(refused(format!("PART {key}"), 400, "InvalidArgument"));
        }
        let mut state = self.upload(key, upload)?;
        let etag = hex(&md5(&body));
        let blob = self.put_blob(&body)?;
        let old = state
            .parts
            .insert(n, (etag.clone(), body.len() as u64, blob));
        self.save_upload(upload, &state)?;
        if let Some((_, _, old)) = old {
            self.drop_blob(&old);
        }
        Ok(etag)
    }

    async fn complete(&self, key: &str, upload: &str, parts: &[(u32, String)]) -> Result<Written> {
        let what = || format!("COMPLETE {key}");
        let state = self.upload(key, upload)?;
        if parts.is_empty() {
            return Err(refused(what(), 400, "MalformedXML"));
        }
        if parts.windows(2).any(|w| w[0].0 >= w[1].0) {
            return Err(refused(what(), 400, "InvalidPartOrder"));
        }
        let mut chosen = Vec::new();
        for (i, (n, etag)) in parts.iter().enumerate() {
            let part = state.parts.get(n).filter(|p| p.0 == etag.trim_matches('"'));
            let Some((etag, size, blob)) = part else {
                return Err(refused(what(), 400, "InvalidPart"));
            };
            if i + 1 < parts.len() && *size < self.min_part {
                return Err(refused(what(), 400, "EntityTooSmall"));
            }
            chosen.push((etag, *size, blob));
        }
        // The parts' bytes, concatenated into a new blob.
        let id = random();
        let temp = self.path("tmp", &id);
        let mut out = std::fs::File::create(&temp)?;
        let mut digests = Vec::new();
        for (etag, _, blob) in &chosen {
            out.write_all(&std::fs::read(self.path("blobs", blob))?)?;
            digests.extend(cluster::unhex(etag).unwrap_or_default());
        }
        drop(out);
        std::fs::rename(&temp, self.path("blobs", &id))?;
        let etag = format!("{}-{}", hex(&md5(&digests)), parts.len());
        let size = chosen.iter().map(|p| p.1).sum();
        let object = new_object(size, etag.clone(), &state.headers, &state.metadata);
        let stored = self.store(key, object, id, None)?;
        self.forget(upload, &state);
        Ok(Written {
            version: stored.as_ref().map(|o| o.version.clone()),
            etag,
            modified_ms: stored.map(|o| o.modified_ms),
        })
    }

    async fn abort(&self, key: &str, upload: &str) -> Result<()> {
        let state = self.upload(key, upload)?;
        self.forget(upload, &state);
        Ok(())
    }

    async fn parts(&self, key: &str, upload: &str) -> Result<Vec<(u32, String, u64)>> {
        let state = self.upload(key, upload)?;
        Ok(state
            .parts
            .into_iter()
            .map(|(n, (etag, size, _))| (n, etag, size))
            .collect())
    }
}

#[cfg(test)]
#[path = "local-test.rs"]
mod tests;
