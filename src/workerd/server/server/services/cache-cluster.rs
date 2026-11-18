// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `cache` service's store: one across the nodes, by the protocol in
//! `cluster_logic::cache`, which this drives. A value is a record spooled to a file in
//! `<path>/cluster-<node>/`, removed once the protocol drops it. Nothing survives a restart: a node
//! locks its directory, and as it starts removes every `cluster-*` directory no node has locked.
//!
//! The protocol runs on a monotonic clock in ms. Its view, `fresh` and `active` come from the
//! registry by wall time (`Cluster::cache_view`), set before every call and every 20 ms. A
//! draining node is not active but stays in views until it leaves: the keys it is home to, about
//! 1/N, miss meanwhile, and their puts are not stored.
//!
//! A message is HTTP to this service on node `to` (`ClusterNode::peer_service`), whose metadata
//! names its sender as `forwardedBy`, which only a peer sets:
//!
//! - `Get`: `GET http://cache/<key>`, with `cf-cache-have: <node> <count>` for the version of a
//!   near copy, answered `cf-cache-reply: value <node> <count>` with the record as the body,
//!   `same` or `miss`.
//! - `Put`: `PUT`, the record as the body; `Delete`: `POST`. Answered `cf-cache-reply: applied
//!   true`, `applied false`, `not-home` or `inactive`.
//!
//! Each request carries `cf-cache-op: <op>`. Its answer waits for the protocol's reply, which it
//! may hold for leases. A request that fails, or is answered otherwise, is lost (`Cache::lost`).
//! A value is spooled whole before the protocol sees it. Messages to this node itself go
//! straight to the protocol.

use std::cell::RefCell;
use std::collections::HashMap;
use std::fs::File;
use std::io::Write;
use std::path::Path;
use std::path::PathBuf;
use std::pin::Pin;
use std::rc::Rc;
use std::time::Duration;
use std::time::Instant;

use cluster_logic::cache::Cache;
use cluster_logic::cache::Done;
use cluster_logic::cache::Hit;
use cluster_logic::cache::Msg;
use cluster_logic::cache::Reply;
use http::HeaderMap;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncOutputStream;
use kj_hyper::io_kj_error;
use kj_rs::KjMaybe;
use tokio::sync::oneshot;

use crate::Result;
use crate::bridge::ffi;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::services::disk::Temporary;
use crate::services::disk::copy_file;
use crate::services::disk::header_value;
use crate::services::disk::kj_headers;
use crate::services::header_table;

type Value = Rc<Temporary>;
/// What awaits each key's outcome.
type Waiters<K, T> = RefCell<HashMap<K, oneshot::Sender<T>>>;

const REPLY: &str = "cf-cache-reply";
/// How often the protocol gets the latest view and its ticks.
const TICK: Duration = Duration::from_millis(20);
/// How long a match waits for its home's answer to begin.
const MATCH_ANSWER: Duration = Duration::from_secs(2);

struct Shared {
    /// This service's, which every node serves.
    name: String,
    node: Rc<ClusterNode>,
    factory: Rc<Factory>,
    dir: PathBuf,
    /// Held while `dir` is in use, so that other nodes leave it be.
    _lock: File,
    start: Instant,
    cache: RefCell<Cache<Value>>,
    /// This node's operations, awaiting what they came to.
    ops: Waiters<u64, Done<Value>>,
    /// Peers' messages, awaiting their replies, by sender and op.
    asked: Waiters<(String, u64), Msg<Value>>,
}

pub struct Clustered(Rc<Shared>);

/// The store of cache service `name` on `node`, its values under `root`.
pub fn store(
    name: &str,
    root: &Path,
    node: &Rc<ClusterNode>,
    factory: &Rc<Factory>,
) -> Result<Clustered> {
    let me = node.cluster.node();
    let (dir, lock) = claim(root, me).map_err(|e| io_kj_error(&e))?;
    node.caching.set(true);
    let shared = Rc::new(Shared {
        name: name.to_owned(),
        node: Rc::clone(node),
        factory: Rc::clone(factory),
        dir,
        _lock: lock,
        start: Instant::now(),
        cache: RefCell::new(Cache::new(me, node.cache_timing)),
        ops: RefCell::default(),
        asked: RefCell::default(),
    });
    let weak = Rc::downgrade(&shared);
    let mut tick = tokio::time::interval(TICK);
    factory.spawn_detached(async move {
        loop {
            tick.tick().await;
            let Some(shared) = weak.upgrade() else {
                return;
            };
            shared.step(|cache| cache.tick(shared.now()));
        }
    });
    Ok(Clustered(shared))
}

/// Node `me`'s directory under `root`, and its lock, held. Nodes may share `root`: one at a time,
/// under its lock, each first removes the `cluster-*` directories whose lock is free, its own from
/// before a restart and those of nodes that stopped.
pub(super) fn claim(root: &Path, me: &str) -> std::io::Result<(PathBuf, File)> {
    let root_lock = File::create(root.join("cluster.lock"))?;
    root_lock.lock()?;
    for entry in std::fs::read_dir(root)? {
        let path = entry?.path();
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned());
        let free = || {
            File::open(path.join("lock"))
                .ok()
                .is_none_or(|f| f.try_lock().is_ok())
        };
        if name.is_some_and(|n| n.starts_with("cluster-")) && free() {
            std::fs::remove_dir_all(&path)?;
        }
    }
    let dir = root.join(format!("cluster-{me}"));
    std::fs::create_dir(&dir)?;
    let lock = File::create(dir.join("lock"))?;
    lock.lock()?;
    Ok((dir, lock))
}

/// `value`'s file, opened, and its length.
fn open(value: &Value) -> Result<(File, u64)> {
    let file = value.open()?;
    let length = file.metadata().map_err(|e| io_kj_error(&e))?.len();
    Ok((file, length))
}

/// `f`, failed if `at` passes first.
async fn by<T, E>(
    at: Option<Instant>,
    f: impl Future<Output = std::result::Result<T, E>>,
) -> Result<T>
where
    crate::Error: From<E>,
{
    let Some(at) = at else {
        return Ok(f.await?);
    };
    let at = tokio::time::Instant::from_std(at);
    let answer = tokio::time::timeout_at(at, f).await;
    Ok(answer.map_err(|_| kj::failed!("no answer in {MATCH_ANSWER:?}"))??)
}

/// Gives the protocol `msg` from `from`, first dropping the entry a match or
/// delete asks for if it expired: a home neither serves nor counts one.
fn deliver(cache: &mut Cache<Value>, from: &str, msg: Msg<Value>) {
    if let Msg::Get { key, .. } | Msg::Delete { key, .. } = &msg
        && cache
            .held(key)
            .is_some_and(|value| expired(value).unwrap_or(false))
    {
        cache.evict(key);
    }
    cache.receive(from, msg);
}

fn expired(value: &Value) -> Result<bool> {
    Ok(!super::fresh(&mut value.open()?, super::now_ms())?)
}

fn op_of(msg: &Msg<Value>) -> u64 {
    match msg {
        Msg::Get { op, .. }
        | Msg::Got { op, .. }
        | Msg::Put { op, .. }
        | Msg::Delete { op, .. }
        | Msg::Wrote { op, .. } => *op,
    }
}

impl Shared {
    fn now(&self) -> u64 {
        u64::try_from(self.start.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    /// Runs `f` on the protocol, given the latest view first, then sends what it sends and
    /// delivers what it did.
    fn step(self: &Rc<Self>, f: impl FnOnce(&mut Cache<Value>)) {
        let me = self.node.cluster.node();
        let (mut asks, mut replies) = (Vec::new(), Vec::new());
        let done = {
            let mut cache = self.cache.borrow_mut();
            let timing = &self.node.cache_timing;
            let (view, fresh, active) = self.node.cluster.cache_view(cluster::now_ms(), timing);
            cache.set(self.now(), view, fresh, active);
            f(&mut cache);
            loop {
                let sends = cache.take_sends();
                if sends.is_empty() {
                    break;
                }
                for send in sends {
                    match send.msg {
                        msg if send.to == me => deliver(&mut cache, me, msg),
                        msg @ (Msg::Got { .. } | Msg::Wrote { .. }) => replies.push((send.to, msg)),
                        msg => asks.push((send.to, msg)),
                    }
                }
            }
            cache.take_done()
        };
        for (op, done) in done {
            if let Some(waiter) = self.ops.borrow_mut().remove(&op) {
                let _ = waiter.send(done);
            }
        }
        for (to, msg) in replies {
            if let Some(waiter) = self.asked.borrow_mut().remove(&(to, op_of(&msg))) {
                let _ = waiter.send(msg);
            }
        }
        for (to, msg) in asks {
            self.factory.spawn_detached(Rc::clone(self).ask(to, msg));
        }
    }

    /// Starts the operation `f` returns the id of, and awaits what it comes to.
    async fn run(self: &Rc<Self>, f: impl FnOnce(&mut Cache<Value>) -> u64) -> Option<Done<Value>> {
        let (waiter, done) = oneshot::channel();
        self.step(|cache| {
            self.ops.borrow_mut().insert(f(cache), waiter);
        });
        done.await.ok()
    }

    /// Sends `msg` to node `to`, then gives the protocol its answer, or tells it the request
    /// was lost.
    async fn ask(self: Rc<Self>, to: String, msg: Msg<Value>) {
        let op = op_of(&msg);
        let timeout = Duration::from_millis(self.node.cache_timing.timeout_ms);
        let answer = tokio::time::timeout(timeout, self.call(&to, msg)).await;
        self.step(|cache| match answer {
            Ok(Ok(msg)) => cache.receive(&to, msg),
            Ok(Err(e)) => {
                tracing::debug!("cluster cache: to {to}: {}", e.description());
                cache.lost(op);
            }
            Err(_) => cache.lost(op),
        });
    }

    async fn call(&self, to: &str, msg: Msg<Value>) -> Result<Msg<Value>> {
        let (method, op, key, have, value) = match msg {
            Msg::Get { op, key, have } => (Method::GET, op, key, have, None),
            Msg::Put { op, key, value } => (Method::PUT, op, key, None, Some(value)),
            Msg::Delete { op, key } => (Method::POST, op, key, None, None),
            Msg::Got { .. } | Msg::Wrote { .. } => return Err(kj::failed!("not a request")),
        };
        let mut headers = HeaderMap::new();
        headers.insert("cf-cache-op", op.into());
        if let Some((node, n)) = have {
            headers.insert("cf-cache-have", header_value(format!("{node} {n}"))?);
        }
        let mut file = value.as_ref().map(open).transpose()?;
        let length = file.as_ref().map_or(0, |(_, length)| *length);
        // A home answers a match at once: no answer by then is a home out of
        // reach, and a miss is always allowed. A write's answer waits out
        // leases, under the op's timeout.
        let deadline = matches!(method, Method::GET).then(|| Instant::now() + MATCH_ANSWER);
        let channel = by(deadline, self.node.peer_service(to, &self.name)).await?;
        let mut metadata = ffi::new_request_metadata(KjMaybe::None, KjMaybe::None);
        ffi::request_metadata_set_forwarded_by(metadata.as_mut(), self.node.cluster.node());
        let headers = kj_headers(header_table(&self.factory), &headers)?;
        let url = format!("http://cache/{key}");
        let interface = channel.start_request(metadata)?;
        let mut call = ffi::http_call(interface, method, &url, &headers, length);
        if let Some((file, _)) = &mut file {
            let mut body = AsyncOutputStream::from(ffi::http_call_body(call.as_mut()));
            copy_file(file, 0, length, &mut body).await?;
        }
        let mut answer = by(deadline, ffi::http_call_response(call, REPLY)).await?;
        let words: Vec<&str> = answer.header.split(' ').collect();
        let bad = || kj::failed!("answered {} {:?}", answer.status, answer.header);
        let hit = |hit| Msg::Got { op, hit };
        let wrote = |reply| Msg::Wrote { op, reply };
        Ok(match (answer.status, method, &words[..]) {
            (200, Method::GET, ["value", node, n]) => {
                let version = (node.to_string(), n.parse().map_err(|_| bad())?);
                hit(Some(Hit::Value(
                    self.spool(answer.body.as_mut()).await?,
                    version,
                )))
            }
            (200, Method::GET, ["same"]) => hit(Some(Hit::Same)),
            (200, Method::GET, ["miss"]) => hit(None),
            (200, Method::PUT | Method::POST, ["applied", had]) => {
                wrote(Reply::Applied(had.parse().map_err(|_| bad())?))
            }
            (200, Method::PUT | Method::POST, ["not-home"]) => wrote(Reply::NotHome),
            (200, Method::PUT | Method::POST, ["inactive"]) => wrote(Reply::Inactive),
            _ => return Err(bad()),
        })
    }

    /// `body`, whole, in a file of its own.
    async fn spool(&self, mut body: Pin<&mut AsyncInputStream>) -> Result<Value> {
        let (temp, mut file) = Temporary::beside(&self.dir.join("value"))?;
        let mut chunk = vec![0; 64 * 1024];
        loop {
            let read = body.as_mut().try_read(&mut chunk, 1).await?;
            if read == 0 {
                return Ok(Rc::new(temp));
            }
            file.write_all(&chunk[..read])
                .map_err(|e| io_kj_error(&e))?;
        }
    }
}

impl Clustered {
    pub(super) async fn get(&self, key: &str) -> Result<Option<File>> {
        match self.0.run(|cache| cache.get(key)).await {
            Some(Done::Match(Some(value))) => value.open().map(Some),
            _ => Ok(None),
        }
    }

    /// A file to write a value to, removed unless [`Clustered::put`] stores it.
    pub(super) fn draft(&self) -> Result<(Temporary, File)> {
        Temporary::beside(&self.0.dir.join("value"))
    }

    /// Makes the value written to `file` that of `key`: whether it was stored, if known.
    pub(super) async fn put(&self, key: &str, temp: Temporary, file: File) -> Option<bool> {
        drop(file);
        let value = Rc::new(temp);
        match self.0.run(|cache| cache.put(key, value)).await {
            Some(Done::Put(stored)) => stored,
            _ => None,
        }
    }

    /// Deletes the value of `key`: whether there was an unexpired one, if known.
    pub(super) async fn delete(&self, key: &str) -> Option<bool> {
        match self.0.run(|cache| cache.delete(key)).await {
            Some(Done::Delete(had)) => had,
            _ => None,
        }
    }

    /// Drops a value of `key` found expired. Not a write: what any reader may do.
    pub(super) fn expire(&self, key: &str) {
        self.0.step(|cache| cache.evict(key));
    }

    /// A message from cluster node `from`, a request that names it as `forwardedBy`.
    pub(super) async fn message(
        &self,
        from: &str,
        method: Method,
        url: &[u8],
        headers: HeadersRef<'_>,
        body: Pin<&mut AsyncInputStream>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let shared = &self.0;
        let bad = || kj::failed!("cluster cache: a malformed message from {from}");
        let text = |name| headers.get_by_name(name).map(std::str::from_utf8);
        let op = match text("cf-cache-op") {
            Some(Ok(op)) => op.parse().map_err(|_| bad())?,
            _ => return Err(bad()),
        };
        let have = match text("cf-cache-have") {
            Some(Ok(have)) => {
                let (node, n) = have.split_once(' ').ok_or_else(bad)?;
                Some((node.to_owned(), n.parse().map_err(|_| bad())?))
            }
            Some(Err(_)) => return Err(bad()),
            None => None,
        };
        let url = std::str::from_utf8(url).map_err(|_| bad())?;
        let key = url.rsplit('/').next().unwrap_or_default().to_owned();
        let msg = match method {
            Method::GET => Msg::Get { op, key, have },
            Method::PUT => Msg::Put {
                op,
                key,
                value: shared.spool(body).await?,
            },
            Method::POST => Msg::Delete { op, key },
            _ => return Err(bad()),
        };
        let (waiter, reply) = oneshot::channel();
        shared
            .asked
            .borrow_mut()
            .insert((from.to_owned(), op), waiter);
        shared.step(|cache| deliver(cache, from, msg));
        let reply = reply
            .await
            .map_err(|_| kj::disconnected!("cluster cache: no reply to {from}"))?;
        let (text, mut file) = match reply {
            Msg::Got { hit, .. } => match hit {
                Some(Hit::Value(value, (node, n))) => {
                    (format!("value {node} {n}"), Some(open(&value)?))
                }
                Some(Hit::Same) => ("same".to_owned(), None),
                None => ("miss".to_owned(), None),
            },
            Msg::Wrote { reply, .. } => match reply {
                Reply::Applied(had) => (format!("applied {had}"), None),
                Reply::NotHome => ("not-home".to_owned(), None),
                Reply::Inactive => ("inactive".to_owned(), None),
            },
            _ => return Err(kj::failed!("cluster cache: not a reply")),
        };
        let mut answer = HeaderMap::new();
        answer.insert(REPLY, header_value(text)?);
        let answer = kj_headers(header_table(&shared.factory), &answer)?;
        let length = file.as_ref().map_or(0, |(_, length)| *length);
        let mut out = response.send(200, "OK", HeadersRef::from(&*answer), Some(length))?;
        match &mut file {
            Some((file, _)) => copy_file(file, 0, length, &mut out).await,
            None => Ok(()),
        }
    }
}
