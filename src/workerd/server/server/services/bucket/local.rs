// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The store of a `bucket` service without a cluster: a directory on local
//! disk, with S3's semantics.
//!
//! For development and tests, not production: see the last paragraph.
//!
//! - `objects/<md5 of key>`: an object's key, size, `ETag`, version, time,
//!   content headers, user metadata and blobs, as JSON. Named by digest: a
//!   key may be longer than a file name, and S3 holds `a` and `a/b` at
//!   once, which a file tree cannot. Read on each call, so processes may
//!   share the directory; a listing reads them all.
//! - `uploads/<id>`: a multipart upload's key, headers, metadata and parts.
//! - `blobs/<id>`: an object's or a part's bytes. A blob is never rewritten:
//!   a write takes a new one and removes the old. A multipart object is its
//!   parts' blobs in order, so completing one copies nothing.
//!
//! Every file is written to `tmp/` first and renamed in, so a reader never
//! sees a partial one. A version is random, so a guard naming an object's
//! version fails once it is rewritten, even with the same bytes.
//!
//! A call that reads before it writes (a guarded write, a delete, a part)
//! holds `lock` meanwhile, so it is atomic across processes. A read that
//! finds a blob gone, its object rewritten meanwhile, reads the object again.
//! Disk I/O is synchronous, as in the `disk` service, and a large write holds
//! the loop while it lands. That suits `workerd serve` and tests; a
//! production store is the cluster's bucket.

use std::collections::BTreeMap;
use std::collections::VecDeque;
use std::io::Read;
use std::io::Seek;
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

/// An upload no part has reached for this long is abandoned.
const STALE: std::time::Duration = std::time::Duration::from_hours(24);

/// An object, as its file in `objects/` holds it.
#[derive(Clone)]
struct Meta {
    key: String,
    object: Object,
    /// Its bytes: blobs and their sizes, in order.
    blobs: Vec<(String, u64)>,
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
    /// The least size of a part but the last, at completion.
    min_part: u64,
    /// Locked around each read-then-write, against other processes.
    lock: std::fs::File,
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
            "metadata": o.metadata, "blobs": self.blobs,
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
            blobs: serde_json::from_value(json["blobs"].clone()).ok()?,
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
        let store = Self {
            root: root.to_owned(),
            min_part,
            lock: std::fs::File::create(root.join("lock"))?,
        };
        store.sweep()?;
        Ok(store)
    }

    /// Drops uploads no part has reached in `STALE`, and their parts, and
    /// files a write left in `tmp/`: a store has no lifecycle rule to.
    fn sweep(&self) -> Result<()> {
        self.locked(|| {
            for dir in ["uploads", "tmp"] {
                for file in std::fs::read_dir(self.root.join(dir))? {
                    let file = file?;
                    let age = file.metadata()?.modified()?.elapsed();
                    if age.is_ok_and(|age| age < STALE) {
                        continue;
                    }
                    let json = std::fs::read(file.path()).unwrap_or_default();
                    let json: Option<Json> = serde_json::from_slice(&json).ok();
                    match json
                        .as_ref()
                        .and_then(Upload::of)
                        .filter(|_| dir == "uploads")
                    {
                        Some(upload) => self.forget(&file.file_name().to_string_lossy(), &upload),
                        None => drop(std::fs::remove_file(file.path())),
                    }
                }
            }
            Ok(())
        })
    }

    /// `f`, with no other process's between its reads and its writes. `f`
    /// never awaits, so no other call of this process's comes between either.
    fn locked<T>(&self, f: impl FnOnce() -> Result<T>) -> Result<T> {
        self.lock.lock()?;
        let out = f();
        self.lock.unlock()?;
        out
    }

    /// The object `key`, as its file holds it now.
    fn meta(&self, key: &str) -> Result<Option<Meta>> {
        let json = match std::fs::read(self.path("objects", &Self::object_file(key))) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            json => json?,
        };
        let meta = serde_json::from_slice(&json).ok();
        Ok(meta.as_ref().and_then(Meta::of).filter(|m| m.key == key))
    }

    /// Every object, by key.
    fn index(&self) -> Result<BTreeMap<String, Meta>> {
        let mut index = BTreeMap::new();
        for file in std::fs::read_dir(self.root.join("objects"))? {
            // A file gone since the listing is an object deleted meanwhile.
            let Ok(json) = std::fs::read(file?.path()) else {
                continue;
            };
            if let Some(meta) = serde_json::from_slice(&json)
                .ok()
                .as_ref()
                .and_then(Meta::of)
            {
                index.insert(meta.key.clone(), meta);
            }
        }
        Ok(index)
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

    fn drop_blobs(&self, blobs: &[(String, u64)]) {
        for (id, _) in blobs {
            self.drop_blob(id);
        }
    }

    fn object_file(key: &str) -> String {
        hex(&md5(key.as_bytes()))
    }

    /// Writes `blobs` at `key` if `guard` holds; otherwise drops them. The
    /// caller holds the lock.
    fn commit(
        &self,
        key: &str,
        mut object: Object,
        blobs: Vec<(String, u64)>,
        guard: Option<&Guard>,
    ) -> Result<Option<Object>> {
        let current = self.meta(key)?;
        let holds = match (guard, &current) {
            (None, _) | (Some(Guard::Absent), None) => true,
            (Some(Guard::Match(version)), Some(meta)) => *version == meta.object.version,
            _ => false,
        };
        if !holds {
            self.drop_blobs(&blobs);
            return Ok(None);
        }
        object.version = Version::new(random());
        object.modified_ms = cluster::now_ms();
        let meta = Meta {
            key: key.to_owned(),
            object: object.clone(),
            blobs,
        };
        let json = meta.to_json().to_string();
        self.save("objects", &Self::object_file(key), json.as_bytes())?;
        if let Some(old) = current {
            self.drop_blobs(&old.blobs);
        }
        Ok(Some(object))
    }

    /// Upload `id` of `key`. An ID is user input (`resumeMultipartUpload`),
    /// so one `begin` couldn't have made is refused before it is a path.
    fn upload(&self, key: &str, id: &str) -> Result<Upload> {
        let ours = id.len() == 32 && id.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
        let json = ours
            .then(|| std::fs::read(self.path("uploads", id)).ok())
            .flatten();
        let json: Option<Json> = json.and_then(|j| serde_json::from_slice(&j).ok());
        let upload = json.as_ref().and_then(Upload::of).filter(|u| u.key == key);
        upload.ok_or_else(|| refused(format!("UPLOAD {key}"), 404, "NoSuchUpload"))
    }

    /// The object `key` and its bytes in `range`, the blobs opened now, so a
    /// write that replaces the object meanwhile doesn't change what they hold.
    fn snapshot(&self, key: &str, range: Option<Range>) -> Result<Option<(Object, Body)>> {
        let Some(Meta {
            mut object, blobs, ..
        }) = self.meta(key)?
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
        let (mut files, mut at, end) = (Vec::new(), 0, offset + length);
        for (id, size) in blobs {
            let (from, to) = (offset.max(at), end.min(at + size));
            if from < to {
                let mut file = std::fs::File::open(self.path("blobs", &id))?;
                file.seek(std::io::SeekFrom::Start(from - at))?;
                files.push((file, to - from));
            }
            at += size;
        }
        Ok(Some((object, chunks(files))))
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

/// The chunks of each file's bytes, from where it is, in turn.
fn chunks(files: Vec<(std::fs::File, u64)>) -> Body {
    let read = |mut files: VecDeque<_>| async move {
        let (file, left): &mut (std::fs::File, u64) = files.front_mut()?;
        let n = usize::try_from(*left).map_or(CHUNK, |n| n.min(CHUNK));
        let mut chunk = vec![0; n];
        if let Err(e) = file.read_exact(&mut chunk) {
            // An error ends the body.
            return Some((Err(Error::Io(e)), VecDeque::new()));
        }
        *left -= n as u64;
        if *left == 0 {
            files.pop_front();
        }
        Some((Ok(Bytes::from(chunk)), files))
    };
    futures::stream::unfold(files.into_iter().collect(), read).boxed_local()
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
        let Some((object, body)) = self.read(key, None).await? else {
            return Ok(None);
        };
        let body: Vec<Bytes> = body
            .collect::<Vec<_>>()
            .await
            .into_iter()
            .collect::<Result<_>>()?;
        let mut body = body.concat();
        if let Some(to) = to {
            std::fs::write(to, std::mem::take(&mut body))?;
        }
        Ok(Some((body, object.version)))
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
        self.locked(|| {
            if let Some(meta) = self.meta(key)? {
                std::fs::remove_file(self.path("objects", &Self::object_file(key)))?;
                self.drop_blobs(&meta.blobs);
            }
            Ok(())
        })
    }

    async fn head(&self, key: &str) -> Result<Option<Object>> {
        Ok(self.meta(key)?.map(|meta| meta.object))
    }

    async fn read(&self, key: &str, range: Option<Range>) -> Result<Option<(Object, Body)>> {
        // A blob gone is most likely an object rewritten since its file was
        // read; reading it again finds the new one.
        let gone = |r: &Result<_>| matches!(r, Err(Error::Io(e)) if e.kind() == std::io::ErrorKind::NotFound);
        let mut read = self.snapshot(key, range);
        for _ in 0..2 {
            if !gone(&read) {
                break;
            }
            read = self.snapshot(key, range);
        }
        read
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
        let index = self.index()?;
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
                    version: o.version.clone(),
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
        let blobs = vec![(self.put_blob(&body)?, body.len() as u64)];
        let object = new_object(body.len() as u64, etag, headers, metadata);
        let stored = self.locked(|| self.commit(key, object, blobs, guard.as_ref()))?;
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
        let etag = hex(&md5(&body));
        let blob = self.put_blob(&body)?;
        let part = (etag.clone(), body.len() as u64, blob);
        let old = self.locked(|| {
            let mut state = self.upload(key, upload)?;
            let old = state.parts.insert(n, part.clone());
            self.save_upload(upload, &state)?;
            Ok(old)
        });
        match old {
            Ok(old) => old.map(|(_, _, old)| self.drop_blob(&old)),
            Err(e) => {
                self.drop_blob(&part.2);
                return Err(e);
            }
        };
        Ok(etag)
    }

    async fn complete(&self, key: &str, upload: &str, parts: &[(u32, String)]) -> Result<Written> {
        self.locked(|| self.complete_locked(key, upload, parts))
    }

    async fn abort(&self, key: &str, upload: &str) -> Result<()> {
        self.locked(|| {
            let state = self.upload(key, upload)?;
            self.forget(upload, &state);
            Ok(())
        })
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

impl Local {
    fn complete_locked(&self, key: &str, upload: &str, parts: &[(u32, String)]) -> Result<Written> {
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
        let digests = chosen
            .iter()
            .flat_map(|p| cluster::unhex(p.0).unwrap_or_default());
        let etag = format!(
            "{}-{}",
            hex(&md5(&digests.collect::<Vec<_>>())),
            parts.len()
        );
        let size = chosen.iter().map(|p| p.1).sum();
        let blobs = chosen.iter().map(|p| (p.2.clone(), p.1)).collect();
        let object = new_object(size, etag.clone(), &state.headers, &state.metadata);
        let stored = self.commit(key, object, blobs, None)?;
        // The object now holds the chosen parts' blobs; the rest go.
        let mut state = state;
        state
            .parts
            .retain(|n, _| !parts.iter().any(|(p, _)| p == n));
        self.forget(upload, &state);
        Ok(Written {
            version: stored.as_ref().map(|o| o.version.clone()),
            etag,
            modified_ms: stored.map(|o| o.modified_ms),
        })
    }
}

#[cfg(test)]
#[path = "local-test.rs"]
mod tests;
