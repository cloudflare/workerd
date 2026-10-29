// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The alarm index: which node wakes which Durable Object, and by when.
//!
//! An object's alarm lives in its own database, which moves with it. The
//! index lets survivors find the alarms of an owner that died with its
//! objects idle. An entry `alarms/<node>/<time>/<namespace>/<actor>/<seq>`
//! says `node` wakes `actor` no later than `time`: it may be early or stale,
//! but never late, since waking an object whose alarm next or went is a
//! no-op. Its body is the actor's name, if it has one, for `ctx.id.name`.
//! `seq` numbers a node's entries, so that deleting a stale one never
//! deletes a later one for the same object and time.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;

use futures::StreamExt;
use futures::stream;

use crate::Bucket;
use crate::Result;
use crate::log::ascending;
use crate::log::parse;
use crate::sha256;

/// One object's alarm, under some node.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub at_ms: u64,
    /// The namespace's hash (`namespace_hash`).
    pub namespace: String,
    pub actor: String,
    pub seq: u64,
}

impl Entry {
    fn key(&self, node: &str) -> String {
        let (at, seq) = (ascending(self.at_ms), ascending(self.seq));
        format!("alarms/{node}/{at}/{}/{}/{seq}", self.namespace, self.actor)
    }
}

pub struct Alarms {
    bucket: Bucket,
}

impl Alarms {
    pub fn new(bucket: Bucket) -> Self {
        Self { bucket }
    }

    pub async fn put(&self, node: &str, entry: &Entry, name: Option<&str>) -> Result<()> {
        let body = name.unwrap_or_default().as_bytes().to_vec();
        self.bucket.put(&entry.key(node), body, &[]).await
    }

    pub async fn delete(&self, node: &str, entry: &Entry) -> Result<()> {
        self.bucket.delete(&entry.key(node)).await
    }

    /// `node`'s entries, earliest first.
    pub async fn list(&self, node: &str) -> Result<Vec<Entry>> {
        let under = format!("alarms/{node}/");
        let listed = self.bucket.list(&under).await?;
        Ok(listed
            .iter()
            .filter_map(|e| {
                let mut parts = e.key.strip_prefix(&under)?.split('/');
                let at_ms = parse(parts.next()?, true)?;
                let namespace = parts.next()?.to_owned();
                let actor = parts.next()?.to_owned();
                let seq = parse(parts.next()?, true)?;
                parts.next().is_none().then_some(Entry {
                    at_ms,
                    namespace,
                    actor,
                    seq,
                })
            })
            .collect())
    }

    /// The name `node`'s entry carries: none without one, or once it is
    /// gone.
    pub async fn name(&self, node: &str, entry: &Entry) -> Result<Option<String>> {
        let got = self.bucket.get(&entry.key(node)).await?;
        Ok(got
            .map(|(body, _)| String::from_utf8_lossy(&body).into_owned())
            .filter(|name| !name.is_empty()))
    }
}

/// Entries a node writes, deletes or adopts at once.
const CONCURRENCY: usize = 32;

/// An object, as the index keys it: namespace hash and actor.
type Key = (String, String);

/// One object's alarm on this node: the entry this node has in the
/// bucket, and the alarm the entry should be for.
#[derive(Default)]
struct Armed {
    indexed: Option<Entry>,
    want: Option<u64>,
    name: Option<String>,
}

/// An alarm this node took over from a dead one, to schedule here.
#[derive(Debug, PartialEq, Eq)]
pub struct Adopted {
    pub namespace: String,
    pub actor: String,
    pub name: Option<String>,
    pub at_ms: u64,
}

/// This node's side of the index.
pub struct Index {
    alarms: Alarms,
    node: String,
    armed: RefCell<BTreeMap<Key, Armed>>,
    /// Entries of this node's to delete.
    stale: RefCell<Vec<Entry>>,
    /// The last entry's `seq`.
    seq: Cell<u64>,
    /// Objects whose alarm fired here while another node owned them: that
    /// owner, and when this node first found it there.
    elsewhere: RefCell<BTreeMap<Key, (String, u64)>>,
    /// How long an owner must outlive that before this node's entry goes:
    /// past its lease, had it died before indexing the alarm itself.
    grace_ms: u64,
}

impl Index {
    pub fn new(bucket: Bucket, node: &str, grace_ms: u64) -> Self {
        Self {
            alarms: Alarms::new(bucket),
            node: node.to_owned(),
            armed: RefCell::default(),
            stale: RefCell::default(),
            seq: Cell::default(),
            elsewhere: RefCell::default(),
            grace_ms,
        }
    }

    fn entry(&self, key: &Key, at_ms: u64) -> Entry {
        self.seq.set(self.seq.get() + 1);
        Entry {
            at_ms,
            namespace: key.0.clone(),
            actor: key.1.clone(),
            seq: self.seq.get(),
        }
    }

    /// Index an object's alarm, `None` once cleared. Resolves once the
    /// bucket holds a new or earlier alarm, so that the commit setting it
    /// is acknowledged after. A later or cleared alarm waits for `tidy`,
    /// its entry early meanwhile, which costs at most a wake.
    pub async fn arm(
        &self,
        namespace: &str,
        actor: &str,
        name: Option<&str>,
        at_ms: Option<u64>,
    ) -> Result<()> {
        let key = (namespace.to_owned(), actor.to_owned());
        self.elsewhere.borrow_mut().remove(&key);
        let indexed = {
            let mut armed = self.armed.borrow_mut();
            let armed = armed.entry(key.clone()).or_default();
            armed.want = at_ms;
            armed.name = name.map(str::to_owned);
            armed.indexed.as_ref().map(|i| i.at_ms)
        };
        let Some(at_ms) = at_ms.filter(|&at| indexed.is_none_or(|i| at < i)) else {
            return Ok(());
        };
        self.put(key, at_ms, name).await
    }

    /// Put this node's entry for `key` at `at_ms`, and keep it unless an
    /// earlier one landed meanwhile.
    async fn put(&self, key: Key, at_ms: u64, name: Option<&str>) -> Result<()> {
        let entry = self.entry(&key, at_ms);
        self.alarms.put(&self.node, &entry, name).await?;
        let mut armed = self.armed.borrow_mut();
        let armed = armed.entry(key).or_default();
        let stale = match &armed.indexed {
            Some(i) if i.at_ms <= at_ms => Some(entry),
            _ => armed.indexed.replace(entry),
        };
        self.stale.borrow_mut().extend(stale);
        Ok(())
    }

    /// Bring this node's entries in line with what `arm` asked for: move
    /// later alarms' entries later, delete cleared ones, and write again
    /// those whose write failed.
    pub async fn tidy(&self) {
        let moves: Vec<_> = {
            let mut armed = self.armed.borrow_mut();
            armed.retain(|_, a| a.indexed.is_some() || a.want.is_some());
            armed
                .iter()
                .filter(|(_, a)| a.indexed.as_ref().map(|i| i.at_ms) != a.want)
                .map(|(key, a)| (key.clone(), a.indexed.clone(), a.want, a.name.clone()))
                .collect()
        };
        stream::iter(moves)
            .for_each_concurrent(CONCURRENCY, |(key, indexed, want, name)| {
                self.move_entry(key, indexed, want, name)
            })
            .await;
        let stale = std::mem::take(&mut *self.stale.borrow_mut());
        stream::iter(stale)
            .for_each_concurrent(CONCURRENCY, |entry| async move {
                if self.alarms.delete(&self.node, &entry).await.is_err() {
                    self.stale.borrow_mut().push(entry);
                }
            })
            .await;
    }

    /// Index `key` at `want`, in place of `indexed`.
    async fn move_entry(
        &self,
        key: Key,
        indexed: Option<Entry>,
        want: Option<u64>,
        name: Option<String>,
    ) {
        let next = want.map(|want| self.entry(&key, want));
        if let Some(next) = &next
            && self
                .alarms
                .put(&self.node, next, name.as_deref())
                .await
                .is_err()
        {
            return;
        }
        let mut armed = self.armed.borrow_mut();
        let Some(a) = armed.get_mut(&key) else {
            return;
        };
        let stale = if a.indexed == indexed && a.want == want {
            std::mem::replace(&mut a.indexed, next)
        } else {
            // Armed meanwhile: the next tidy moves what this did not.
            next
        };
        self.stale.borrow_mut().extend(stale);
        if a.indexed.is_none() && a.want.is_none() {
            armed.remove(&key);
        }
    }

    /// An alarm of this node's fired for an object `owner` owns. That node
    /// indexed the alarm when it opened the object, unless it died first,
    /// which its lease hides for a while: this node keeps its entry until
    /// the same owner has outlived the grace since this node first found it
    /// there. Answers whether to look again later.
    pub async fn elsewhere(
        &self,
        namespace: &str,
        actor: &str,
        owner: &str,
        now_ms: u64,
    ) -> Result<bool> {
        let key = (namespace.to_owned(), actor.to_owned());
        let first = {
            let mut elsewhere = self.elsewhere.borrow_mut();
            let seen = elsewhere
                .entry(key)
                .or_insert_with(|| (owner.to_owned(), now_ms));
            if seen.0 != owner {
                *seen = (owner.to_owned(), now_ms);
            }
            seen.1
        };
        if now_ms < first + self.grace_ms {
            return Ok(true);
        }
        self.arm(namespace, actor, None, None).await?;
        Ok(false)
    }

    /// Take over this node's share of `dead` nodes' alarms among the `live`
    /// ones: each is indexed here before it goes there, so it is never in
    /// neither. Hands `adopted` every one to schedule here as it goes,
    /// whatever this node knows of the object: its index says what it
    /// wakes, not that its scheduler still holds it, since a wake that
    /// finds no alarm consumes the one but not the other. An entry that
    /// fails stays where it is, for the next round.
    pub async fn adopt(&self, live: &[String], dead: &[String], mut adopted: impl FnMut(Adopted)) {
        for node in dead {
            let entries = match self.alarms.list(node).await {
                Ok(entries) => entries,
                Err(e) => {
                    tracing::warn!("listing {node}'s alarms: {e}");
                    continue;
                }
            };
            let mine = entries
                .into_iter()
                .filter(|e| assignee(live, e) == Some(self.node.as_str()));
            let mut results = stream::iter(mine)
                .map(|e| self.adopt_one(node, e))
                .buffer_unordered(CONCURRENCY);
            while let Some(result) = results.next().await {
                match result {
                    Ok(a) => adopted(a),
                    Err(e) => tracing::warn!("adopting {node}'s alarm: {e}"),
                }
            }
        }
    }

    /// Index `entry` here, then delete it from `node`. `armed` changes only
    /// once the entry is indexed, so a failure leaves it to adopt again.
    async fn adopt_one(&self, node: &str, entry: Entry) -> Result<Adopted> {
        let name = self.alarms.name(node, &entry).await?;
        let key = (entry.namespace.clone(), entry.actor.clone());
        let (known, indexed) = {
            let armed = self.armed.borrow();
            let armed = armed.get(&key);
            (
                armed.is_some(),
                armed.and_then(|a| a.indexed.as_ref().map(|i| i.at_ms)),
            )
        };
        if indexed.is_none_or(|i| entry.at_ms < i) {
            self.put(key.clone(), entry.at_ms, name.as_deref()).await?;
        }
        if !known {
            let mut armed = self.armed.borrow_mut();
            let armed = armed.entry(key).or_default();
            armed.want = Some(entry.at_ms);
            armed.name.clone_from(&name);
        }
        if let Err(e) = self.alarms.delete(node, &entry).await {
            tracing::warn!("deleting {node}'s adopted alarm: {e}");
        }
        Ok(Adopted {
            namespace: entry.namespace,
            actor: entry.actor,
            name,
            at_ms: entry.at_ms,
        })
    }
}

/// Which of the `live` nodes adopts `entry` from a dead one: the one with
/// the highest hash of node and object (rendezvous hashing). Every node
/// agrees without talking, and a node's death moves only its share.
pub fn assignee<'a>(live: &'a [String], entry: &Entry) -> Option<&'a str> {
    live.iter()
        .max_by_key(|node| {
            let data = format!("{node}\0{}\0{}", entry.namespace, entry.actor);
            sha256(data.as_bytes())
        })
        .map(String::as_str)
}

#[cfg(test)]
#[path = "alarms-test.rs"]
mod tests;
