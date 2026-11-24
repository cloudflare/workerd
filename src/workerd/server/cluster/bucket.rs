// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::Path;
use std::rc::Rc;
use std::time::Duration;

use bytes::Bytes;

use crate::Error;
use crate::Result;
pub use crate::http::Payload;

/// The most writes `Bucket::put_if_settled` makes of one write whose
/// refusals may be of its own copies in progress.
pub const SETTLE_WRITES: u32 = 3;

/// The identity of one stored version of an object: what a conditional
/// write names to replace exactly that version: an S3 `ETag`, a Cloud
/// Storage generation.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Version(pub(crate) String);

impl Version {
    /// A version as a store answered it, carried by a caller and handed
    /// back to guard a write.
    pub const fn new(version: String) -> Self {
        Self(version)
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

/// The precondition of a conditional write.
#[derive(Clone)]
pub enum Guard {
    /// The object must not exist.
    Absent,
    /// The object must still be this version.
    Match(Version),
}

/// One listed object; `key` is relative to the bucket's prefix.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Entry {
    pub key: String,
    pub size: u64,
    /// As `Object::etag`.
    pub etag: String,
    /// As `Object::version`: the version a `head` would answer.
    pub version: Version,
    pub modified_ms: u64,
}

/// One page of a listing (`Store::page`).
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Page {
    pub entries: Vec<Entry>,
    /// The common prefixes, each up to and including the delimiter.
    pub prefixes: Vec<String>,
    /// Where the next page starts, if the listing goes on.
    pub token: Option<String>,
}

/// The content headers a write stores with an object and a read answers.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Headers {
    pub content_type: Option<String>,
    pub content_language: Option<String>,
    pub content_disposition: Option<String>,
    pub content_encoding: Option<String>,
    pub cache_control: Option<String>,
}

impl Headers {
    /// Each header's name, and its value if set.
    pub fn named(&self) -> [(&'static str, Option<&str>); 5] {
        [
            ("content-type", self.content_type.as_deref()),
            ("content-language", self.content_language.as_deref()),
            ("content-disposition", self.content_disposition.as_deref()),
            ("content-encoding", self.content_encoding.as_deref()),
            ("cache-control", self.cache_control.as_deref()),
        ]
    }

    /// The headers `value` answers for each name.
    pub fn from_fn(value: impl Fn(&str) -> Option<String>) -> Self {
        Self {
            content_type: value("content-type"),
            content_language: value("content-language"),
            content_disposition: value("content-disposition"),
            content_encoding: value("content-encoding"),
            cache_control: value("cache-control"),
        }
    }
}

/// A stored object, as a HEAD or a read describes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Object {
    /// The whole object's size, even when a read answers a range of it.
    pub size: u64,
    /// What a write names to replace exactly this object (`Guard::Match`).
    pub version: Version,
    /// The store's `ETag`, unquoted: the MD5 of an object written in one
    /// request; on S3 `<MD5 of the parts' MD5s>-<parts>` for a multipart
    /// one. Only S3 fences on it, as its version: a write conditional on an
    /// `ETag` on every store HEADs for the version first.
    pub etag: String,
    pub modified_ms: u64,
    pub headers: Headers,
    /// User metadata, in the order the store answers it, named without the
    /// dialect's prefix and in lower case, as HTTP carries names.
    pub metadata: Vec<(String, String)>,
    /// The offset and length of the bytes a ranged read answered; `None`
    /// when they are the whole object.
    pub range: Option<(u64, u64)>,
}

/// What a write answers of the object it wrote.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Written {
    /// Always there for a write; for a multipart completion, only where the
    /// store answers it: S3's is the `ETag`, but Cloud Storage may not
    /// answer the generation, which a HEAD then reads.
    pub version: Option<Version>,
    /// As `Object::etag`.
    pub etag: String,
    /// The object's time, where the store answers it: S3 and Cloud Storage
    /// do not.
    pub modified_ms: Option<u64>,
}

/// The bytes of an object a read asks for, as a `Range` header names them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Range {
    /// `length` bytes from `offset`, or to the end; a `length` is not 0.
    From { offset: u64, length: Option<u64> },
    /// The last bytes, this many.
    Suffix(u64),
}

impl Range {
    pub fn header(self) -> String {
        match self {
            Self::From { offset, length } => {
                let end = length.map_or(String::new(), |n| {
                    offset.saturating_add(n).saturating_sub(1).to_string()
                });
                format!("bytes={offset}-{end}")
            }
            Self::Suffix(n) => format!("bytes=-{n}"),
        }
    }

    /// The offset and length of the bytes this is of an object of `size`
    /// bytes, as S3 serves them: `None` for none (416).
    pub fn of(self, size: u64) -> Option<(u64, u64)> {
        match self {
            Self::From { offset, .. } if offset >= size => None,
            Self::From { offset, length } => Some((
                offset,
                length.map_or(size - offset, |n| n.min(size - offset)),
            )),
            Self::Suffix(n) if n == 0 || size == 0 => None,
            Self::Suffix(n) => Some((size - n.min(size), n.min(size))),
        }
    }
}

/// An object's body as a read streams it, chunk by chunk. A body that fails
/// partway ends with the error.
pub type Body = futures::stream::LocalBoxStream<'static, Result<Bytes>>;

/// An object store: the calls the cluster makes, with the semantics S3 and
/// Cloud Storage share.
#[async_trait::async_trait(?Send)]
pub trait Store {
    /// The object and its version; with `to`, the body goes to that file
    /// instead.
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>>;
    /// `Ok(None)` when the guard failed and nothing was written;
    /// `Error::OutcomeUnknown` when the guard failed but an earlier attempt
    /// of the call, or a copy sent beside it, may have written. A write
    /// without a guard answers neither: the store refusing it is another
    /// error.
    async fn put(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        metadata: &[(&str, String)],
    ) -> Result<Option<Version>>;
    async fn list(&self, prefix: &str) -> Result<Vec<Entry>>;
    /// The distinct prefixes under `prefix` up to and including the next
    /// `/`, in lexical order: a listing with delimiter `/`.
    async fn prefixes(&self, prefix: &str) -> Result<Vec<String>> {
        let listed = self.list(prefix).await?;
        let mut prefixes: Vec<_> = listed
            .iter()
            .filter_map(|e| {
                let (next, _) = e.key.strip_prefix(prefix)?.split_once('/')?;
                Some(format!("{prefix}{next}/"))
            })
            .collect();
        prefixes.sort();
        prefixes.dedup();
        Ok(prefixes)
    }
    /// Idempotent: deleting an absent key succeeds.
    async fn delete(&self, key: &str) -> Result<()>;
    /// See `Client::clock_skew_ms`.
    fn clock_skew_ms(&self) -> Option<i64> {
        None
    }

    // The calls R2 makes of an object mapped onto a store object. The other
    // stores (the tests' fault injectors) refuse them. A store's error
    // answers are `Error::Refused`.

    /// The object, or `None` when it does not exist.
    async fn head(&self, key: &str) -> Result<Option<Object>> {
        Err(unsupported("HEAD", key))
    }
    /// The object, or `range` of it, with its body left to read. A range of
    /// none of it is refused with 416.
    async fn read(&self, key: &str, _range: Option<Range>) -> Result<Option<(Object, Body)>> {
        Err(unsupported("GET", key))
    }
    /// One page of the keys under `prefix` after `start_after`, at most
    /// `max` keys and common prefixes, those with `delimiter` after `prefix`
    /// rolled up into their prefix up to it; from `token`, a page's `token`,
    /// for the next.
    async fn page(
        &self,
        prefix: &str,
        _start_after: Option<&str>,
        _delimiter: Option<&str>,
        _max: usize,
        _token: Option<&str>,
    ) -> Result<Page> {
        Err(unsupported("LIST", prefix))
    }
    /// `put`, with content headers, answering the `ETag` too.
    async fn write(
        &self,
        key: &str,
        _body: Payload,
        _guard: Option<Guard>,
        _headers: &Headers,
        _metadata: &[(String, String)],
    ) -> Result<Option<Written>> {
        Err(unsupported("PUT", key))
    }
    /// Starts a multipart upload of `key`, whose object gets these headers
    /// and metadata: answers its ID.
    async fn begin(
        &self,
        key: &str,
        _headers: &Headers,
        _metadata: &[(String, String)],
    ) -> Result<String> {
        Err(unsupported("BEGIN", key))
    }
    /// Uploads part `n` (1 to 10,000) of `upload`, replacing any before:
    /// answers its `ETag`, unquoted. Every part but the last is at least 5
    /// MiB.
    async fn part(&self, key: &str, _upload: &str, _n: u32, _body: Bytes) -> Result<String> {
        Err(unsupported("PART", key))
    }
    /// Writes the object of `upload` from `parts`, numbers ascending with
    /// their `ETag`s, unconditionally: a completion takes no precondition.
    async fn complete(
        &self,
        key: &str,
        _upload: &str,
        _parts: &[(u32, String)],
    ) -> Result<Written> {
        Err(unsupported("COMPLETE", key))
    }
    /// Drops `upload` and its parts.
    async fn abort(&self, key: &str, _upload: &str) -> Result<()> {
        Err(unsupported("ABORT", key))
    }
    /// The parts of `upload` so far, ascending: number, `ETag` unquoted,
    /// size.
    async fn parts(&self, key: &str, _upload: &str) -> Result<Vec<(u32, String, u64)>> {
        Err(unsupported("PARTS", key))
    }
}

fn unsupported(call: &str, key: &str) -> Error {
    Error::Http(format!("{call} {key}: not supported by this store"))
}

/// A store's answer `status` and error `code` to the request `what`.
pub fn refused(what: String, status: u16, code: &str) -> Error {
    let code = code.to_owned();
    Error::Refused { what, status, code }
}

/// An object store under a key prefix. Clones share the store.
#[derive(Clone)]
pub struct Bucket {
    store: Rc<dyn Store>,
    prefix: String,
}

impl Bucket {
    pub fn new(store: impl Store + 'static, prefix: &str) -> Self {
        Self {
            store: Rc::new(store),
            prefix: normalize(prefix),
        }
    }

    /// The same store under another prefix.
    #[must_use]
    pub fn at(&self, prefix: &str) -> Self {
        Self {
            store: self.store.clone(),
            prefix: normalize(prefix),
        }
    }

    #[cfg(test)]
    pub fn memory(prefix: &str) -> Self {
        Self::new(memory::Memory::default(), prefix)
    }

    pub fn clock_skew_ms(&self) -> Option<i64> {
        self.store.clock_skew_ms()
    }

    pub(crate) fn prefix(&self) -> &str {
        &self.prefix
    }

    fn key(&self, key: &str) -> String {
        format!("{}{key}", self.prefix)
    }

    /// The object and its version, or `None` when it does not exist.
    pub async fn get(&self, key: &str) -> Result<Option<(Vec<u8>, Version)>> {
        self.store.get(&self.key(key), None).await
    }

    /// Write `body` if the guard holds. `Ok(None)` means the guard failed and
    /// nothing was written; an error means the write may have landed.
    /// `Error::OutcomeUnknown` is the store refusing the guard where the
    /// refusal may be of this write's own earlier attempt or other copy.
    pub async fn put_if(&self, key: &str, body: Vec<u8>, guard: Guard) -> Result<Option<Version>> {
        let body = Payload::Bytes(body.into());
        self.store.put(&self.key(key), body, Some(guard), &[]).await
    }

    /// `put_if`, with an unknown outcome (`Error::OutcomeUnknown`) settled by
    /// reading the object back: the write counts as landed, at the version
    /// read, if the object holds `body`, and as refused if it holds another
    /// version than the guarded one. If it still holds the guarded version,
    /// or is still absent, the refusal was of a copy of this write that the
    /// bucket had not yet applied, and may yet: one sent before a lost
    /// connection or a 503 (`Client::exchange`), or beside it
    /// (`http::prompt`), which R2 answers with a conflict (409) while it is
    /// in progress. The write is then sent again under the same guard after
    /// `pause`, of which at most one copy lands, and a refusal of it is
    /// settled the same way, up to `SETTLE_WRITES` writes in all; after
    /// those, or when a write sent again or the read after it fails, the
    /// outcome stays unknown. For writes whose bytes no other writer puts
    /// at `key`, or where it means the same if one does, and whose body
    /// differs from what the guarded version holds: on a store whose
    /// versions do not follow content, a refused write of those same bytes
    /// would read back as landed.
    pub async fn put_if_settled(
        &self,
        key: &str,
        body: Vec<u8>,
        guard: Guard,
        pause: Duration,
    ) -> Result<Option<Version>> {
        let mut writes = 0;
        loop {
            writes += 1;
            // Once a write's outcome is unknown, it stays so whatever fails.
            let unsettled = |error: Error| match error {
                error @ Error::OutcomeUnknown(_) => error,
                error if writes > 1 => Error::OutcomeUnknown(format!(
                    "PUT {key}: {error}, after a write of unknown outcome"
                )),
                error => error,
            };
            let unknown = match self.put_if(key, body.clone(), guard.clone()).await {
                Err(unknown @ Error::OutcomeUnknown(_)) => unknown,
                // Perhaps refused against an earlier copy that landed since.
                Ok(None) if writes > 1 => Error::OutcomeUnknown(format!(
                    "PUT {key}: refused after a write of unknown outcome"
                )),
                Err(error) => return Err(unsettled(error)),
                written => return written,
            };
            let current = self.get(key).await.map_err(unsettled)?;
            let guarded = match (&guard, &current) {
                (Guard::Absent, None) => true,
                (Guard::Match(guarded), Some((_, version))) => version == guarded,
                _ => false,
            };
            match current {
                Some((current, version)) if current == body => return Ok(Some(version)),
                _ if !guarded => return Ok(None),
                _ if writes == SETTLE_WRITES => return Err(unknown),
                _ => tokio::time::sleep(pause).await,
            }
        }
    }

    /// Write `body` unconditionally, with user metadata.
    pub async fn put(&self, key: &str, body: Vec<u8>, metadata: &[(&str, String)]) -> Result<()> {
        let key = self.key(key);
        let body = Payload::Bytes(body.into());
        self.store.put(&key, body, None, metadata).await.map(drop)
    }

    /// Writes the object to the file `to`: `false` when it does not exist.
    pub async fn get_file(&self, key: &str, to: &Path) -> Result<bool> {
        let got = self.store.get(&self.key(key), Some(to)).await?;
        Ok(got.is_some())
    }

    /// Writes the file `from` unconditionally, with user metadata.
    pub async fn put_file(
        &self,
        key: &str,
        from: &Path,
        metadata: &[(&str, String)],
    ) -> Result<()> {
        let body = Payload::File(from.to_owned());
        self.store
            .put(&self.key(key), body, None, metadata)
            .await
            .map(drop)
    }

    /// The keys under `prefix` in lexical order.
    pub async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
        let listed = self.store.list(&self.key(prefix)).await?;
        Ok(listed
            .into_iter()
            .filter_map(|entry| self.ours(entry))
            .collect())
    }

    /// `entry` with its key relative to the prefix, if under it.
    fn ours(&self, entry: Entry) -> Option<Entry> {
        let key = entry.key.strip_prefix(&self.prefix)?.to_owned();
        Some(Entry { key, ..entry })
    }

    /// See `Store::prefixes`.
    pub async fn prefixes(&self, prefix: &str) -> Result<Vec<String>> {
        let listed = self.store.prefixes(&self.key(prefix)).await?;
        let ours = listed.iter().filter_map(|p| p.strip_prefix(&self.prefix));
        Ok(ours.map(str::to_owned).collect())
    }

    /// Idempotent: deleting an absent key succeeds.
    pub async fn delete(&self, key: &str) -> Result<()> {
        self.store.delete(&self.key(key)).await
    }

    /// See `Store::head`.
    pub async fn head(&self, key: &str) -> Result<Option<Object>> {
        self.store.head(&self.key(key)).await
    }

    /// See `Store::read`.
    pub async fn read(&self, key: &str, range: Option<Range>) -> Result<Option<(Object, Body)>> {
        self.store.read(&self.key(key), range).await
    }

    /// See `Store::page`; keys, `start_after` and prefixes are relative to
    /// the bucket's prefix.
    pub async fn page(
        &self,
        prefix: &str,
        start_after: Option<&str>,
        delimiter: Option<&str>,
        max: usize,
        token: Option<&str>,
    ) -> Result<Page> {
        let (prefix, start_after) = (self.key(prefix), start_after.map(|key| self.key(key)));
        let page = self
            .store
            .page(&prefix, start_after.as_deref(), delimiter, max, token);
        let page = page.await?;
        let entries = page.entries.into_iter().filter_map(|e| self.ours(e));
        let prefixes = page
            .prefixes
            .iter()
            .filter_map(|p| p.strip_prefix(&self.prefix));
        Ok(Page {
            entries: entries.collect(),
            prefixes: prefixes.map(str::to_owned).collect(),
            token: page.token,
        })
    }

    /// See `Store::write`.
    pub async fn write(
        &self,
        key: &str,
        body: Payload,
        guard: Option<Guard>,
        headers: &Headers,
        metadata: &[(String, String)],
    ) -> Result<Option<Written>> {
        let key = self.key(key);
        self.store.write(&key, body, guard, headers, metadata).await
    }

    /// See `Store::begin`.
    pub async fn begin(
        &self,
        key: &str,
        headers: &Headers,
        metadata: &[(String, String)],
    ) -> Result<String> {
        self.store.begin(&self.key(key), headers, metadata).await
    }

    /// See `Store::part`.
    pub async fn part(&self, key: &str, upload: &str, n: u32, body: Bytes) -> Result<String> {
        self.store.part(&self.key(key), upload, n, body).await
    }

    /// See `Store::complete`.
    pub async fn complete(
        &self,
        key: &str,
        upload: &str,
        parts: &[(u32, String)],
    ) -> Result<Written> {
        self.store.complete(&self.key(key), upload, parts).await
    }

    /// See `Store::abort`.
    pub async fn abort(&self, key: &str, upload: &str) -> Result<()> {
        self.store.abort(&self.key(key), upload).await
    }

    /// See `Store::parts`.
    pub async fn parts(&self, key: &str, upload: &str) -> Result<Vec<(u32, String, u64)>> {
        self.store.parts(&self.key(key), upload).await
    }
}

fn normalize(prefix: &str) -> String {
    let prefix = prefix.trim_matches('/');
    if prefix.is_empty() {
        String::new()
    } else {
        format!("{prefix}/")
    }
}

#[cfg(test)]
pub mod memory {
    use std::cell::Cell;
    use std::cell::RefCell;
    use std::collections::BTreeMap;
    use std::path::Path;
    use std::rc::Rc;

    use super::Entry;
    use super::Guard;
    use super::Payload;
    use super::Store;
    use super::Version;
    use crate::Error;
    use crate::Result;

    /// S3's semantics for the calls the cluster makes, in memory.
    #[derive(Default)]
    pub struct Memory {
        objects: RefCell<BTreeMap<String, Stored>>,
        writes: Cell<u64>,
        /// As GCS's S3 API treats `If-Match` on a PUT.
        pub ignores_guards: Cell<bool>,
        /// As a store whose `ETag`s are weak treats `If-Match`.
        pub never_matches: Cell<bool>,
        /// Keys whose next guarded write answers `Error::OutcomeUnknown`
        /// whether or not it was written, as the client does when a retry's
        /// guard fails after an attempt that may have landed; one entry per
        /// write. Shared, so a test keeps a handle on it once a `Bucket`
        /// owns the store.
        pub unknown: Rc<RefCell<Vec<String>>>,
        /// Another writer's write of a key, which lands just before the next
        /// guarded write of that key is decided; one entry per write.
        pub interloper: Rc<RefCell<Vec<Write>>>,
    }

    /// A write's key and body.
    pub type Write = (String, Vec<u8>);

    /// An object's body, version and modified time.
    type Stored = (Vec<u8>, Version, u64);

    /// Removes the first entry of `entries` that `matches` and answers it.
    fn take<T>(entries: &RefCell<Vec<T>>, matches: impl Fn(&T) -> bool) -> Option<T> {
        let mut entries = entries.borrow_mut();
        let i = entries.iter().position(matches)?;
        Some(entries.remove(i))
    }

    impl Memory {
        /// A version counts writes, as no store's does, so that nothing
        /// takes an `ETag` for one.
        fn next_version(&self) -> Version {
            self.writes.set(self.writes.get() + 1);
            Version(format!("\"{}\"", self.writes.get()))
        }

        fn insert(&self, key: &str, body: Vec<u8>) -> Version {
            let version = self.next_version();
            let object = (body, version.clone(), crate::now_ms());
            self.objects.borrow_mut().insert(key.to_owned(), object);
            version
        }
    }

    #[async_trait::async_trait(?Send)]
    impl Store for Memory {
        async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
            let got = self
                .objects
                .borrow()
                .get(key)
                .map(|(body, version, _)| (body.clone(), version.clone()));
            match (got, to) {
                (Some((body, version)), Some(to)) => {
                    std::fs::write(to, body)?;
                    Ok(Some((Vec::new(), version)))
                }
                (got, _) => Ok(got),
            }
        }

        async fn put(
            &self,
            key: &str,
            body: Payload,
            guard: Option<Guard>,
            _metadata: &[(&str, String)],
        ) -> Result<Option<Version>> {
            let body = match body {
                Payload::Bytes(bytes) => bytes.to_vec(),
                Payload::File(path) => std::fs::read(path)?,
            };
            let guarded = guard.is_some();
            let unknown = guarded && take(&self.unknown, |k| k == key).is_some();
            if guarded && let Some((_, other)) = take(&self.interloper, |(k, _)| k == key) {
                self.insert(key, other);
            }
            let current = self.objects.borrow().get(key).map(|o| o.1.clone());
            let holds = match guard {
                None => true,
                Some(Guard::Absent) => current.is_none(),
                Some(Guard::Match(version)) => {
                    current == Some(version) && !self.never_matches.get()
                }
            };
            let outcome_unknown =
                || Err(Error::OutcomeUnknown(format!("PUT {key}: 412 (simulated)")));
            if !holds && !self.ignores_guards.get() {
                return if unknown { outcome_unknown() } else { Ok(None) };
            }
            let version = self.insert(key, body);
            if unknown {
                return outcome_unknown();
            }
            Ok(Some(version))
        }

        async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
            let objects = self.objects.borrow();
            let listed = objects.iter().filter(|(key, _)| key.starts_with(prefix));
            let listed = listed.map(|(key, (body, version, modified_ms))| Entry {
                key: key.clone(),
                size: body.len() as u64,
                etag: crate::hex(&crate::md5(body)),
                version: version.clone(),
                modified_ms: *modified_ms,
            });
            Ok(listed.collect())
        }

        async fn delete(&self, key: &str) -> Result<()> {
            self.objects.borrow_mut().remove(key);
            Ok(())
        }
    }
}

#[cfg(test)]
#[path = "bucket-test.rs"]
mod tests;
