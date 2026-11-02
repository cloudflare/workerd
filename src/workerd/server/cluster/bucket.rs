// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::path::Path;
use std::rc::Rc;

use crate::Result;
pub use crate::http::Payload;

/// The identity of one stored version of an object: what a conditional
/// write names to replace exactly that version. An S3 `ETag`.
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

/// An object store: S3's semantics for the calls the cluster makes.
#[async_trait::async_trait(?Send)]
pub trait Store {
    /// The object and its version; with `to`, the body goes to that file
    /// instead.
    async fn get(&self, key: &str, to: Option<&Path>) -> Result<Option<(Vec<u8>, Version)>>;
    /// `Ok(None)` when the guard failed and nothing was written.
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
    /// See `S3::clock_skew_ms`.
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
    pub async fn put_if(&self, key: &str, body: Vec<u8>, guard: Guard) -> Result<Option<Version>> {
        let body = Payload::Bytes(body.into());
        self.store.put(&self.key(key), body, Some(guard), &[]).await
    }

    /// Write `body` unconditionally, with `x-amz-meta-*` metadata.
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

    /// Writes the file `from` unconditionally, with `x-amz-meta-*` metadata.
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

    use super::Entry;
    use super::Guard;
    use super::Payload;
    use super::Store;
    use super::Version;
    use crate::Result;

    /// S3's semantics for the calls the cluster makes, in memory.
    #[derive(Default)]
    pub struct Memory {
        objects: RefCell<BTreeMap<String, (Vec<u8>, Version)>>,
        writes: Cell<u64>,
        /// As GCS's S3 API treats `If-Match` on a PUT.
        pub ignores_guards: Cell<bool>,
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
            let mut objects = self.objects.borrow_mut();
            let current = objects.get(key).map(|(_, version)| version);
            let holds = match guard {
                None => true,
                Some(Guard::Absent) => current.is_none(),
                Some(Guard::Match(version)) => current == Some(&version),
            };
            if !holds && !self.ignores_guards.get() {
                return Ok(None);
            }
            self.writes.set(self.writes.get() + 1);
            let version = Version(format!("\"{}\"", self.writes.get()));
            objects.insert(key.to_owned(), (body, version.clone()));
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
