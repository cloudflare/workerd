// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::Path;
use std::rc::Rc;

use crate::Error;
use crate::Result;
pub use crate::http::Payload;

/// The identity of one stored version of an object: what a conditional
/// write names to replace exactly that version: an S3 `ETag`, a Cloud
/// Storage generation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Version(pub(crate) String);

/// The precondition of a conditional write.
pub enum Guard {
    /// The object must not exist.
    Absent,
    /// The object must still be this version.
    Match(Version),
}

/// One listed object; `key` is relative to the bucket's prefix.
pub struct Entry {
    pub key: String,
}

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
    /// read, if the object holds `body`, and as refused otherwise. For
    /// writes whose bytes no other writer puts at `key`, or where it means
    /// the same if one does, and whose body differs from what the guarded
    /// version holds: on a store whose versions do not follow content, a
    /// refused write of those same bytes would read back as landed. Not for
    /// a write `http::prompt` may send twice (`http::Repeat::GuardedWrite`):
    /// the other copy can land after the read.
    pub async fn put_if_settled(
        &self,
        key: &str,
        body: Vec<u8>,
        guard: Guard,
    ) -> Result<Option<Version>> {
        match self.put_if(key, body.clone(), guard).await {
            Err(Error::OutcomeUnknown(_)) => {}
            written => return written,
        }
        Ok(match self.get(key).await? {
            Some((current, version)) if current == body => Some(version),
            _ => None,
        })
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
            .filter_map(|entry| {
                let key = entry.key.strip_prefix(&self.prefix)?.to_owned();
                Some(Entry { key })
            })
            .collect())
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
        objects: RefCell<BTreeMap<String, (Vec<u8>, Version)>>,
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

    /// Removes the first entry of `entries` that `matches` and answers it.
    fn take<T>(entries: &RefCell<Vec<T>>, matches: impl Fn(&T) -> bool) -> Option<T> {
        let mut entries = entries.borrow_mut();
        let i = entries.iter().position(matches)?;
        Some(entries.remove(i))
    }

    #[async_trait::async_trait(?Send)]
    impl Store for Memory {
        async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>> {
            let got = self.objects.borrow().get(key).cloned();
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
            _: &[(&str, String)],
        ) -> Result<Option<Version>> {
            let body = match body {
                Payload::Bytes(bytes) => bytes.to_vec(),
                Payload::File(path) => std::fs::read(path)?,
            };
            let guarded = guard.is_some();
            let unknown = guarded && take(&self.unknown, |k| k == key).is_some();
            let mut objects = self.objects.borrow_mut();
            if guarded && let Some((_, other)) = take(&self.interloper, |(k, _)| k == key) {
                self.writes.set(self.writes.get() + 1);
                let version = Version(format!("\"{}\"", self.writes.get()));
                objects.insert(key.to_owned(), (other, version));
            }
            let current = objects.get(key).map(|(_, version)| version);
            let holds = match guard {
                None => true,
                Some(Guard::Absent) => current.is_none(),
                Some(Guard::Match(version)) => {
                    current == Some(&version) && !self.never_matches.get()
                }
            };
            let outcome_unknown =
                || Err(Error::OutcomeUnknown(format!("PUT {key}: 412 (simulated)")));
            if !holds && !self.ignores_guards.get() {
                return if unknown { outcome_unknown() } else { Ok(None) };
            }
            self.writes.set(self.writes.get() + 1);
            let version = Version(format!("\"{}\"", self.writes.get()));
            objects.insert(key.to_owned(), (body, version.clone()));
            if unknown {
                return outcome_unknown();
            }
            Ok(Some(version))
        }

        async fn list(&self, prefix: &str) -> Result<Vec<Entry>> {
            let objects = self.objects.borrow();
            let keys = objects.keys().filter(|key| key.starts_with(prefix));
            Ok(keys.map(|key| Entry { key: key.clone() }).collect())
        }

        async fn delete(&self, key: &str) -> Result<()> {
            self.objects.borrow_mut().remove(key);
            Ok(())
        }
    }
}
