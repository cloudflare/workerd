// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Built-in KV behind `kvNamespaceId` bindings (`kv/kv.js`).
//!
//! The rewrite runs on the serialized config before anything reads it. A
//! config that binds a `kvNamespaceId` gets the Worker `workerd:kv`, whose
//! `KVNamespace` objects hold each namespace's index in the disk
//! `Config.builtinStorage` names, so in a cluster they replicate and fail
//! over as any object. A `kvNamespaceId = id` binding is a KV binding to
//! that Worker with props `{"namespace": id}` (bindings.rs).
//!
//! Values over the inline limit are blobs in `workerd:kv-blobs`, which takes
//! GET, PUT and DELETE on `/<dir>/b/<id>`: a directory beside the objects'
//! databases, or in a cluster the bucket at `<prefix>kv/`, since a node's
//! disk does not outlive the node. A node keeps the blobs it writes and
//! reads in a cache on its disk (up to 1 GiB); a blob
//! never changes, so the cache needs no invalidation.

use std::cell::RefCell;
use std::path::PathBuf;
use std::pin::Pin;
use std::rc::Rc;
use std::time::SystemTime;

use capnp::message;
use capnp::message::ReaderOptions;
use cluster::Bucket;
use hashlink::LinkedHashMap;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service as HttpService;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use worker::AlarmResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::config;
use workerd_capnp::service;
use workerd_capnp::worker::binding;

use crate::Result;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::Service;
use crate::config::capnp_error;
use crate::config::text;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::unsupported;

const WORKER: &str = include_str!("../../kv/kv.js");
/// The Worker `kvNamespaceId` bindings call.
pub const NAME: &str = "workerd:kv";
const BLOBS: &str = "workerd:kv-blobs";
const UNIQUE_KEY: &str = "workerd-KVNamespace";
const BLOB_CACHE: &str = "workerd-KVNamespace-blob-cache";
const BLOB_CACHE_BYTES: u64 = 1024 * 1024 * 1024;

/// `config` with `workerd:kv` if a Worker binds a `kvNamespaceId`;
/// unchanged when none does.
pub fn with_builtin(config: Vec<u64>) -> Result<Vec<u64>> {
    rewrite(&config)
        .map_err(capnp_error)
        .map(|c| c.unwrap_or(config))
}

fn rewrite(words: &[u64]) -> capnp::Result<Option<Vec<u64>>> {
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_ne_bytes()).collect();
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    let reader = capnp::serialize::read_message_from_flat_slice(&mut &bytes[..], options)?;
    let old = reader.get_root::<config::Reader>()?;
    let Some(disk) = disk(old)? else {
        return Ok(None);
    };
    let mut message = message::Builder::new_default();
    message.set_root(old)?;
    let root = message.get_root::<config::Builder>()?;
    let old_services = old.get_services()?;
    let n = old_services.len();
    let mut services = root.init_services(n + 1);
    for i in 0..n {
        services.set_with_caveats(i, old_services.get(i))?;
    }
    add_worker(services.get(n), &disk);
    Ok(Some(
        capnp::serialize::write_message_to_words(&message)
            .chunks_exact(8)
            .map(|chunk| {
                let mut word = [0; 8];
                word.copy_from_slice(chunk);
                u64::from_ne_bytes(word)
            })
            .collect(),
    ))
}

fn add_worker(mut service: service::Builder<'_>, disk: &str) {
    service.set_name(NAME);
    let mut w = service.init_worker();
    let mut module = w.reborrow().init_modules(1).get(0);
    module.set_name("kv.js");
    module.set_es_module(WORKER);
    w.set_compatibility_date("2026-01-01");
    let mut class = w.reborrow().init_durable_object_namespaces(1).get(0);
    class.set_class_name("KVNamespace");
    class.set_unique_key(UNIQUE_KEY);
    class.set_enable_sql(true);
    w.reborrow()
        .init_durable_object_storage()
        .set_local_disk(disk);
    let mut bindings = w.init_bindings(2);
    let mut shard = bindings.reborrow().get(0);
    shard.set_name("SHARD");
    shard
        .init_durable_object_namespace()
        .set_class_name("KVNamespace");
    let mut blobs = bindings.get(1);
    blobs.set_name("BLOBS");
    blobs.init_service().set_name(BLOBS);
}

/// `Config.builtinStorage`, if a Worker binds a `kvNamespaceId`; `None`
/// when none does. Names starting `workerd:` are the built-ins', which the
/// factory compiles with experimental features and their own compatibility
/// date, so a config may not define one.
fn disk(config: config::Reader<'_>) -> capnp::Result<Option<String>> {
    let fail = |m: String| Err(capnp::Error::failed(m));
    for service in config.get_services()? {
        let name = service.get_name()?.to_str()?;
        if name.starts_with("workerd:") {
            return fail(format!(
                "Service \"{name}\": names starting with \"workerd:\" are \
                 reserved for workerd's built-ins."
            ));
        }
    }
    for service in config.get_services()? {
        let service::Which::Worker(w) = service.which()? else {
            continue;
        };
        for b in w?.get_bindings()? {
            if !matches!(b.which()?, binding::Which::KvNamespaceId(_)) {
                continue;
            }
            if !config.has_builtin_storage() {
                let name = service.get_name()?.to_str()?;
                return fail(format!(
                    "Worker \"{name}\" binds a kvNamespaceId, which needs \
                     Config.builtinStorage."
                ));
            }
            return Ok(Some(config.get_builtin_storage()?.to_string()?));
        }
    }
    Ok(None)
}

/// `workerd:kv-blobs` if the config has `workerd:kv`: in a cluster the
/// bucket, else a directory in `Config.builtinStorage`.
pub fn make_blob_service(
    config: config::Reader<'_>,
    services: &LinkedHashMap<String, Service>,
    cluster: Option<&Rc<ClusterNode>>,
    factory: &Rc<Factory>,
) -> Result<Option<(String, Service)>> {
    if !services.contains_key(NAME) {
        return Ok(None);
    }
    let disk = text(config.get_builtin_storage())?;
    let service = match (cluster, services.get(&disk)) {
        (Some(node), Some(Service::Disk(d))) => {
            let Some(path) = d.writable_path() else {
                return Err(kj::failed!("Disk \"{disk}\" must be writable."));
            };
            let dir = PathBuf::from(path).join(BLOB_CACHE);
            let cache = BlobCache::new(dir, BLOB_CACHE_BYTES)?;
            let bucket = node.cluster.kv_blobs();
            let factory = Rc::clone(factory);
            let blobs = Blobs {
                bucket,
                factory,
                cache,
            };
            Service::Leaf(Rc::new(BucketBlobs(Rc::new(blobs))))
        }
        (None, Some(Service::Disk(d))) => {
            Service::Disk(d.subdirectory(&format!("{UNIQUE_KEY}-blobs"))?)
        }
        // The objects' storage reports the missing disk.
        (None, _) => return Ok(None),
        (Some(_), _) => {
            return Err(kj::failed!("Config.builtinStorage must name a disk."));
        }
    };
    Ok(Some((BLOBS.to_owned(), service)))
}

// =======================================================================================
// Blobs in the bucket

struct Blobs {
    bucket: Bucket,
    factory: Rc<Factory>,
    cache: BlobCache,
}

/// Blobs in files of a directory, by path, at most `cap` bytes of them: the
/// least recently used go first. The directory is emptied at startup, so
/// the index is in memory only.
struct BlobCache {
    dir: PathBuf,
    cap: u64,
    lru: RefCell<(LinkedHashMap<String, u64>, u64)>,
}

impl BlobCache {
    fn new(dir: PathBuf, cap: u64) -> Result<Self> {
        let io = |e: std::io::Error| kj::failed!("{}: {e}", dir.display());
        if let Err(e) = std::fs::remove_dir_all(&dir)
            && e.kind() != std::io::ErrorKind::NotFound
        {
            return Err(io(e));
        }
        std::fs::create_dir_all(&dir).map_err(io)?;
        let lru = RefCell::new((LinkedHashMap::new(), 0));
        Ok(Self { dir, cap, lru })
    }

    /// A blob path (`is_blob`) as a file name.
    fn file(&self, path: &str) -> PathBuf {
        self.dir.join(path.replace('/', "-"))
    }

    fn get(&self, path: &str) -> Option<Vec<u8>> {
        self.lru.borrow_mut().0.to_back(path)?;
        let value = std::fs::read(self.file(path)).ok();
        if value.is_none() {
            self.delete(path);
        }
        value
    }

    fn put(&self, path: &str, value: &[u8]) {
        let len = value.len() as u64;
        if len > self.cap {
            return;
        }
        let temp = self.dir.join(format!(".{}.tmp", path.replace('/', "-")));
        let wrote =
            std::fs::write(&temp, value).and_then(|()| std::fs::rename(&temp, self.file(path)));
        if wrote.is_err() {
            let _ = std::fs::remove_file(&temp);
            return;
        }
        let mut lru = self.lru.borrow_mut();
        let (map, bytes) = &mut *lru;
        *bytes = *bytes + len - map.insert(path.to_owned(), len).unwrap_or(0);
        while *bytes > self.cap {
            let Some((old, n)) = map.pop_front() else {
                break;
            };
            let _ = std::fs::remove_file(self.file(&old));
            *bytes -= n;
        }
    }

    fn delete(&self, path: &str) {
        let mut lru = self.lru.borrow_mut();
        let (map, bytes) = &mut *lru;
        if let Some(n) = map.remove(path) {
            *bytes -= n;
            let _ = std::fs::remove_file(self.file(path));
        }
    }
}

struct BucketBlobs(Rc<Blobs>);

impl Channel for BucketBlobs {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(BlobRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("KV blobs"))
    }
}

struct BlobRequest(Rc<Blobs>);

/// Whether `path` is `<dir>/b/<id>` in lowercase hex, as kv.js names blobs.
fn is_blob(path: &str) -> bool {
    let hex = |s: &str| !s.is_empty() && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'));
    path.split_once("/b/")
        .is_some_and(|(dir, id)| hex(dir) && hex(id))
}

async fn read_body(mut body: Pin<&mut AsyncInputStream>) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut buffer = vec![0; 64 * 1024];
    loop {
        let read = body.as_mut().try_read(&mut buffer, 1).await?;
        if read == 0 {
            return Ok(out);
        }
        out.extend_from_slice(&buffer[..read]);
    }
}

fn bucket_error(e: &cluster::Error) -> crate::Error {
    kj::failed!("{e}")
}

#[async_trait::async_trait(?Send)]
impl HttpService for BlobRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        _headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let url = String::from_utf8_lossy(url);
        let rest = url.split_once("://").map_or(&*url, |(_, rest)| rest);
        let path = rest.find('/').map_or("", |i| &rest[i + 1..]);
        let Blobs { bucket, cache, .. } = &*self.0;
        let (status, out) = match method {
            _ if !is_blob(path) => (400, Vec::new()),
            Method::GET => match cache.get(path) {
                Some(value) => (200, value),
                None => match bucket.get(path).await.map_err(|e| bucket_error(&e))? {
                    Some((value, _)) => {
                        cache.put(path, &value);
                        (200, value)
                    }
                    None => (404, Vec::new()),
                },
            },
            Method::PUT => {
                let value = read_body(body).await?;
                bucket
                    .put(path, value.clone(), &[])
                    .await
                    .map_err(|e| bucket_error(&e))?;
                cache.put(path, &value);
                (200, Vec::new())
            }
            Method::DELETE => {
                cache.delete(path);
                bucket.delete(path).await.map_err(|e| bucket_error(&e))?;
                (200, Vec::new())
            }
            _ => (405, Vec::new()),
        };
        let mut map = http::HeaderMap::new();
        map.insert(
            http::header::CONTENT_LENGTH,
            header_value(out.len().to_string())?,
        );
        let kj = kj_headers(header_table(&self.0.factory), &map)?;
        let text = http::StatusCode::from_u16(status)
            .ok()
            .and_then(|s| s.canonical_reason())
            .unwrap_or("");
        let len = out.len() as u64;
        let mut stream =
            response.send(u32::from(status), text, HeadersRef::from(&*kj), Some(len))?;
        stream.write(&out).await
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(unsupported("KV blobs"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for BlobRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("KV blobs"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("KV blobs"))
    }
}

#[cfg(test)]
#[path = "kv-test.rs"]
mod tests;
