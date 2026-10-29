// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The alarm index: which node wakes which Durable Object, and by when.
//!
//! An object's alarm lives in its own database, which moves with it. The
//! index lets survivors find the alarms of an owner that died with its
//! objects idle. An entry `alarms/<node>/<time>/<namespace>/<actor>` says
//! `node` wakes `actor` no later than `time`: it may be early or stale, but
//! never late, since waking an object whose alarm moved or went is a no-op.
//! Its body is the actor's name, if it has one, for `ctx.id.name`.

use std::cell::RefCell;
use std::collections::HashMap;

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
}

impl Entry {
    fn key(&self, node: &str) -> String {
        let at = ascending(self.at_ms);
        format!("alarms/{node}/{at}/{}/{}", self.namespace, self.actor)
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
                parts.next().is_none().then_some(Entry {
                    at_ms,
                    namespace,
                    actor,
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

/// Entries a node adopts at once.
const ADOPT_CONCURRENCY: usize = 32;

/// An object, as the index keys it: namespace hash and actor.
type Key = (String, String);

/// One object's alarm on this node: the entry this node has in the
/// bucket, and the alarm the entry should be for.
#[derive(Default)]
struct Armed {
    indexed: Option<u64>,
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
    armed: RefCell<HashMap<Key, Armed>>,
    /// Entries of this node's to delete.
    stale: RefCell<Vec<Entry>>,
}

impl Index {
    pub fn new(bucket: Bucket, node: &str) -> Self {
        Self {
            alarms: Alarms::new(bucket),
            node: node.to_owned(),
            armed: RefCell::default(),
            stale: RefCell::default(),
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
        let indexed = {
            let mut armed = self.armed.borrow_mut();
            let armed = armed.entry(key.clone()).or_default();
            armed.want = at_ms;
            armed.name = name.map(str::to_owned);
            armed.indexed
        };
        let Some(at_ms) = at_ms.filter(|&at| indexed.is_none_or(|i| at < i)) else {
            return Ok(());
        };
        self.put(key, at_ms, name).await
    }

    /// Put this node's entry for `key` at `at_ms`, and keep it unless an
    /// earlier one landed meanwhile.
    async fn put(&self, key: Key, at_ms: u64, name: Option<&str>) -> Result<()> {
        let entry = Entry {
            at_ms,
            namespace: key.0.clone(),
            actor: key.1.clone(),
        };
        self.alarms.put(&self.node, &entry, name).await?;
        let mut armed = self.armed.borrow_mut();
        let armed = armed.entry(key).or_default();
        match armed.indexed {
            Some(i) if i <= at_ms => {
                if i != at_ms {
                    self.stale.borrow_mut().push(entry);
                }
            }
            old => {
                if let Some(old) = old {
                    self.stale.borrow_mut().push(Entry {
                        at_ms: old,
                        ..entry
                    });
                }
                armed.indexed = Some(at_ms);
            }
        }
        Ok(())
    }

    /// Bring this node's entries in line with what `arm` asked for: move
    /// later alarms' entries later and delete cleared ones.
    pub async fn tidy(&self) {
        let moves: Vec<_> = {
            let mut armed = self.armed.borrow_mut();
            armed.retain(|_, a| a.indexed.is_some() || a.want.is_some());
            armed
                .iter()
                .filter_map(|(key, a)| {
                    let indexed = a.indexed?;
                    let later = a.want.is_none_or(|want| want > indexed);
                    later.then(|| (key.clone(), indexed, a.want, a.name.clone()))
                })
                .collect()
        };
        for (key, indexed, want, name) in moves {
            let entry = Entry {
                at_ms: want.unwrap_or(indexed),
                namespace: key.0.clone(),
                actor: key.1.clone(),
            };
            if want.is_some()
                && self
                    .alarms
                    .put(&self.node, &entry, name.as_deref())
                    .await
                    .is_err()
            {
                continue;
            }
            let mut armed = self.armed.borrow_mut();
            let Some(a) = armed.get_mut(&key) else {
                continue;
            };
            if a.indexed == Some(indexed) {
                a.indexed = want;
                self.stale.borrow_mut().push(Entry {
                    at_ms: indexed,
                    ..entry
                });
            } else if want.is_some() {
                // An earlier alarm was indexed meanwhile.
                self.stale.borrow_mut().push(entry);
            }
            if a.indexed.is_none() && a.want.is_none() {
                armed.remove(&key);
            }
        }
        let stale = std::mem::take(&mut *self.stale.borrow_mut());
        for entry in stale {
            if self.alarms.delete(&self.node, &entry).await.is_err() {
                self.stale.borrow_mut().push(entry);
            }
        }
    }

    /// Take over this node's share of `dead` nodes' alarms among the `live`
    /// ones: each is indexed here before it goes there, so it is never in
    /// neither. Answers those to schedule here; an object this node already
    /// knows keeps the alarm it has.
    pub async fn adopt(&self, live: &[String], dead: &[String]) -> Result<Vec<Adopted>> {
        let mut adopted = Vec::new();
        for node in dead {
            let entries = self.alarms.list(node).await?;
            let mine = entries
                .into_iter()
                .filter(|e| assignee(live, e) == Some(self.node.as_str()));
            let results: Vec<_> = stream::iter(mine)
                .map(|e| self.adopt_one(node, e))
                .buffer_unordered(ADOPT_CONCURRENCY)
                .collect()
                .await;
            for result in results {
                adopted.extend(result?);
            }
        }
        Ok(adopted)
    }

    async fn adopt_one(&self, node: &str, entry: Entry) -> Result<Option<Adopted>> {
        let name = self.alarms.name(node, &entry).await?;
        let key = (entry.namespace.clone(), entry.actor.clone());
        let (known, indexed) = {
            let mut armed = self.armed.borrow_mut();
            let known = armed.contains_key(&key);
            let armed = armed.entry(key.clone()).or_default();
            if !known {
                armed.want = Some(entry.at_ms);
                armed.name.clone_from(&name);
            }
            (known, armed.indexed)
        };
        if indexed.is_none_or(|i| entry.at_ms < i) {
            self.put(key, entry.at_ms, name.as_deref()).await?;
        }
        self.alarms.delete(node, &entry).await?;
        Ok((!known).then_some(Adopted {
            namespace: entry.namespace,
            actor: entry.actor,
            name,
            at_ms: entry.at_ms,
        }))
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
