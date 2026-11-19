// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The cluster's Cache API store: which node holds an entry, and when a
//! write is done. No bucket operations beyond the leases nodes already hold.
//!
//! A key's home is the node rendezvous hashing ranks first in a node's view.
//! A node holds a key's entry, which matches and writes go to, only while it
//! is active (`Timing`) and the key's home in its own view, and drops it once
//! its view moves the key away. So at most one node holds a key's entry at a
//! time: an active node registered longer ago than any fresh view may be
//! stale, so two active nodes list each other, and rendezvous ranks one above
//! the other in both views.
//!
//! Other nodes serve a key only from near copies, each within its lease. A
//! hit leases the value for `lease_ms` from when the asker sent the request,
//! by its own clock. The asker serves a near copy that long, then renews it
//! with a request that names the version it holds, which the home answers
//! without the value if unchanged. The home acks a write only once the leases
//! it granted on the key have run out, so no copy of the old value outlives
//! the write. For the same reason a match issued while a fetch of its key is
//! in flight, within the lease that fetch's answer brings, shares the answer.
//! Leases need no handover: a node that stops serving a key does so long
//! before another starts, as a joining node waits `join_ms`, a fenced node
//! stops before its record expires, and a leaving node expires its record
//! only `lease_wait_ms` after it stopped.
//!
//! A delete whose target is not active goes on down the ranking. An active
//! node that answers the key is not its own proves no node holds it. A put to
//! a home that is not active is not stored.
//!
//! The executor sets `fresh` and `active` by `Timing`, treats a node missing
//! from a fresh view as fenced, and keeps nothing across a restart, after
//! which the node registers again.
//!
//! A caller that keeps what a match returns asks with [`Cache::get_leased`],
//! and may serve it until [`Cache::until`]: its near copy's lease, or on the
//! key's home a lease the home grants itself, which writes wait out as they
//! do the leases it gave other nodes.
//!
//! Safety, which `cache-test.rs` checks by simulation: once a delete, or a put
//! that stored, is done, no match issued later returns a value whose put
//! stored before that write began. A miss is always allowed.

use std::cmp::Reverse;
use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::HashSet;

use crate::handoff::successor;
use crate::handoff::weight;

/// How fresh a view must be, how long writes wait, how long leases last.
#[derive(Clone, Copy, Debug)]
pub struct Timing {
    /// A view whose listing began longer ago than this is stale: the node
    /// serves nothing and starts no writes.
    pub stale_ms: u64,
    /// A node is active only this long after its registration returned: by
    /// then every fresh view lists it, with clocks 1% apart.
    pub join_ms: u64,
    /// A match or write not done by then fails.
    pub timeout_ms: u64,
    /// How long a near copy is served. Shorter than the time between a fenced
    /// node's stop and its record's expiry.
    pub lease_ms: u64,
}

impl Timing {
    /// Listings come at most 11 s apart (see below), so 15 s leaves 4 s for
    /// a busy event loop; `join_ms` outlasts it by a listing, a lease and a
    /// registration.
    pub const DEFAULT: Self = Self {
        stale_ms: 15_000,
        join_ms: 25_000,
        timeout_ms: 30_000,
        lease_ms: 1_000,
    };

    /// `DEFAULT` with every time divided by `divisor`, for tests, as
    /// `lease::Timing::divided`. `None` if `divisor` is 0 or rounds the
    /// lease, the shortest, down to 0.
    #[must_use]
    pub fn divided(divisor: u64) -> Option<Self> {
        let t = Self::DEFAULT;
        let divided = Self {
            stale_ms: t.stale_ms.checked_div(divisor)?,
            join_ms: t.join_ms / divisor,
            timeout_ms: t.timeout_ms / divisor,
            lease_ms: t.lease_ms / divisor,
        };
        (divided.lease_ms > 0).then_some(divided)
    }

    /// Whether a view whose listing began at `listed` is fresh at `now`.
    #[must_use]
    pub fn fresh(&self, now: u64, listed: Option<u64>) -> bool {
        listed.is_some_and(|at| now.saturating_sub(at) < self.stale_ms)
    }

    /// Whether a node registered at `registered` may serve.
    #[must_use]
    pub fn active(&self, now: u64, listed: Option<u64>, registered: Option<u64>) -> bool {
        self.fresh(now, listed)
            && registered.is_some_and(|at| now.saturating_sub(at) >= self.join_ms)
    }

    /// How long a home waits out a lease it granted, with clocks 1% apart.
    #[must_use]
    pub fn lease_wait_ms(&self) -> u64 {
        self.lease_ms + self.lease_ms / 25 + 1
    }
}

// `Cluster::maintain` renews (at most `renew_timeout_ms`), lists (at most
// `list_timeout_ms`) and sleeps `interval_ms`, and a listing counts from its
// start once it returns: a node whose bucket answers in time never goes
// stale.
const _: () = {
    let (t, l) = (Timing::DEFAULT, crate::lease::Timing::DEFAULT);
    let gap = 2 * l.list_timeout_ms + l.interval_ms + l.renew_timeout_ms;
    assert!(t.stale_ms >= gap + 4_000);
};

/// A value's version: the home that stored it, and a count there.
pub type Version = (String, u64);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Msg<V> {
    /// A match; `have`, the version of an expired near copy.
    Get {
        op: u64,
        key: String,
        have: Option<Version>,
    },
    Got {
        op: u64,
        hit: Option<Hit<V>>,
    },
    Put {
        op: u64,
        key: String,
        value: V,
    },
    Delete {
        op: u64,
        key: String,
    },
    Wrote {
        op: u64,
        reply: Reply,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Hit<V> {
    Value(V, Version),
    /// The version the asker has, leased again.
    Same,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reply {
    /// Done; whether the key held a value.
    Applied(bool),
    /// Active, but the key's home in this node's view is another.
    NotHome,
    /// Serves nothing.
    Inactive,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Send<V> {
    pub to: String,
    pub msg: Msg<V>,
}

/// What an operation came to; `None` for a write that failed or whose
/// outcome is unknown.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Done<V> {
    Match(Option<V>),
    Put(Option<bool>),
    Delete(Option<bool>),
}

struct Near<V> {
    value: V,
    version: Version,
    until: u64,
}

enum Op {
    Fetch {
        key: String,
        sent: u64,
        have: Option<Version>,
        /// Matches the answer also answers.
        joined: Vec<u64>,
    },
    Put {
        home: String,
    },
    Delete {
        key: String,
        ranking: Vec<String>,
        at: usize,
    },
}

pub struct Cache<V> {
    me: String,
    timing: Timing,
    now: u64,
    view: Vec<String>,
    fresh: bool,
    active: bool,
    next: u64,
    home: HashMap<String, (V, Version)>,
    /// Until when copies this node gave out of each key may be served. Kept
    /// while inactive: those copies are still out.
    leases: HashMap<String, u64>,
    /// Copies of entries homed elsewhere: served until `until`, then kept ten
    /// leases more to revalidate by version.
    near: HashMap<String, Near<V>>,
    ops: BTreeMap<u64, (u64, Op)>,
    /// Matches by `get_leased` not yet answered, and until when those
    /// answered with a value may serve it.
    leasing: HashSet<u64>,
    untils: HashMap<u64, u64>,
    /// Acks of writes, held until the key's leases run out.
    held: Vec<(u64, Send<V>)>,
    sends: Vec<Send<V>>,
    done: Vec<(u64, Done<V>)>,
}

impl<V: Clone> Cache<V> {
    #[must_use]
    pub fn new(me: &str, timing: Timing) -> Self {
        Self {
            me: me.to_owned(),
            timing,
            now: 0,
            view: Vec::new(),
            fresh: false,
            active: false,
            next: 0,
            home: HashMap::new(),
            leases: HashMap::new(),
            near: HashMap::new(),
            ops: BTreeMap::new(),
            leasing: HashSet::new(),
            untils: HashMap::new(),
            held: Vec::new(),
            sends: Vec::new(),
            done: Vec::new(),
        }
    }

    /// The node `key` lives at, by this node's view.
    #[must_use]
    pub fn home_of(&self, key: &str) -> Option<&str> {
        successor(key, self.view.iter().map(String::as_str))
    }

    /// The latest view, and what this node may do with it. Entries whose key
    /// the view moves away are dropped, and every one while inactive.
    pub fn set(&mut self, now: u64, view: Vec<String>, fresh: bool, active: bool) {
        self.now = now;
        self.fresh = fresh;
        self.active = active;
        if !active {
            self.home.clear();
        }
        if view == self.view {
            return;
        }
        self.view = view;
        let (me, view) = (&self.me, &self.view);
        self.home
            .retain(|key, _| successor(key, view.iter().map(String::as_str)) == Some(me));
        // A node that left the view fenced: writes stop waiting for it.
        let gone: Vec<u64> = self
            .ops
            .iter()
            .filter(|(_, (_, op))| match op {
                Op::Put { home } => !view.contains(home),
                Op::Delete { ranking, at, .. } => !view.contains(&ranking[*at]),
                Op::Fetch { .. } => false,
            })
            .map(|(id, _)| *id)
            .collect();
        for id in gone {
            self.advance(id);
        }
    }

    /// Sends the acks whose leases ran out, drops stale near copies, and
    /// fails what has waited past the timeout.
    pub fn tick(&mut self, now: u64) {
        self.now = now;
        let (ready, held) = std::mem::take(&mut self.held)
            .into_iter()
            .partition::<Vec<_>, _>(|(at, _)| *at <= now);
        self.held = held;
        self.sends.extend(ready.into_iter().map(|(_, send)| send));
        self.leases.retain(|_, until| *until > now);
        let keep = 10 * self.timing.lease_ms;
        self.near.retain(|_, near| near.until + keep > now);
        let timeout = self.timing.timeout_ms;
        let late: Vec<u64> = self
            .ops
            .iter()
            .filter(|(_, (since, _))| now.saturating_sub(*since) > timeout)
            .map(|(id, _)| *id)
            .collect();
        for id in late {
            let done = match self.ops.remove(&id) {
                Some((_, Op::Fetch { joined, .. })) => {
                    self.fetched(id, joined, None, now);
                    continue;
                }
                Some((_, Op::Put { .. })) => Done::Put(None),
                Some((_, Op::Delete { .. })) => Done::Delete(None),
                None => continue,
            };
            self.done.push((id, done));
        }
    }

    /// The transport knows that `op`'s message or its answer will not
    /// arrive, say a refused connection. A match misses now; its late answer
    /// is ignored, as after a timeout. A put's outcome is unknown now: waiting
    /// would end the same way. A delete waits on, to go on down the ranking
    /// once its target leaves the view; going on now could reach an active
    /// node that answers `NotHome` while the target still serves the key.
    pub fn lost(&mut self, op: u64) {
        match self.ops.remove(&op) {
            Some((_, Op::Fetch { joined, .. })) => self.fetched(op, joined, None, 0),
            Some((_, Op::Put { .. })) => self.done.push((op, Done::Put(None))),
            Some(delete) => {
                self.ops.insert(op, delete);
            }
            None => {}
        }
    }

    /// The entry of `key` this node holds as its home.
    #[must_use]
    pub fn held(&self, key: &str) -> Option<&V> {
        self.home.get(key).map(|(value, _)| value)
    }

    /// The entries this node holds as their home.
    pub fn homed(&self) -> impl Iterator<Item = (&str, &V)> {
        self.home
            .iter()
            .map(|(key, (value, _))| (key.as_str(), value))
    }

    /// Drops an entry or near copy, to make room.
    pub fn evict(&mut self, key: &str) {
        self.home.remove(key);
        self.near.remove(key);
    }

    pub fn take_sends(&mut self) -> Vec<Send<V>> {
        std::mem::take(&mut self.sends)
    }

    pub fn take_done(&mut self) -> Vec<(u64, Done<V>)> {
        std::mem::take(&mut self.done)
    }

    /// A match; its result comes as `Done::Match` under the returned id.
    pub fn get(&mut self, key: &str) -> u64 {
        self.get_with(key, false)
    }

    /// A match whose caller keeps the value it returns, until [`Cache::until`].
    pub fn get_leased(&mut self, key: &str) -> u64 {
        self.get_with(key, true)
    }

    /// Until when the caller of `get_leased` match `id`, answered with a value,
    /// may serve it.
    pub fn until(&mut self, id: u64) -> Option<u64> {
        self.untils.remove(&id)
    }

    fn get_with(&mut self, key: &str, leased: bool) -> u64 {
        let id = self.id();
        if leased {
            self.leasing.insert(id);
        }
        let now = self.now;
        if let Some(near) = self.near.get(key).filter(|n| now < n.until) {
            let (value, until) = (near.value.clone(), near.until);
            self.matched(id, Some(value), until);
            return id;
        }
        if self.serves(key) {
            let value = self.home.get(key).map(|(value, _)| value.clone());
            if leased && value.is_some() {
                let lease = self.leases.entry(key.to_owned()).or_default();
                *lease = (*lease).max(now + self.timing.lease_wait_ms());
            }
            self.matched(id, value, now + self.timing.lease_ms);
            return id;
        }
        let lease = self.timing.lease_ms;
        let fetch = self.ops.values_mut().find_map(|(_, op)| match op {
            Op::Fetch {
                key: k,
                sent,
                joined,
                ..
            } if k == key && now < *sent + lease => Some(joined),
            _ => None,
        });
        if let Some(joined) = fetch {
            joined.push(id);
            return id;
        }
        match self.home_of(key).map(str::to_owned) {
            Some(home) if self.fresh && home != self.me => {
                let have = self.near.get(key).map(|n| n.version.clone());
                let key = key.to_owned();
                let msg = Msg::Get {
                    op: id,
                    key: key.clone(),
                    have: have.clone(),
                };
                self.sends.push(Send { to: home, msg });
                let op = Op::Fetch {
                    key,
                    sent: now,
                    have,
                    joined: Vec::new(),
                };
                self.ops.insert(id, (now, op));
            }
            _ => self.matched(id, None, now),
        }
        id
    }

    /// Answers match `id` with `value`, servable until `until` if leased.
    fn matched(&mut self, id: u64, value: Option<V>, until: u64) {
        if self.leasing.remove(&id) && value.is_some() {
            self.untils.insert(id, until);
        }
        self.done.push((id, Done::Match(value)));
    }

    pub fn put(&mut self, key: &str, value: V) -> u64 {
        let id = self.id();
        self.near.remove(key);
        let home = self.home_of(key).map(str::to_owned);
        let Some(home) = home.filter(|_| self.fresh) else {
            self.done.push((id, Done::Put(None)));
            return id;
        };
        let key = key.to_owned();
        let msg = Msg::Put { op: id, key, value };
        self.sends.push(Send {
            to: home.clone(),
            msg,
        });
        self.ops.insert(id, (self.now, Op::Put { home }));
        id
    }

    pub fn delete(&mut self, key: &str) -> u64 {
        let id = self.id();
        self.near.remove(key);
        if !self.fresh || self.view.is_empty() {
            self.done.push((id, Done::Delete(None)));
            return id;
        }
        let mut ranking = self.view.clone();
        ranking.sort_by_key(|node| Reverse((weight(key, node), node.clone())));
        let key = key.to_owned();
        let msg = Msg::Delete {
            op: id,
            key: key.clone(),
        };
        self.sends.push(Send {
            to: ranking[0].clone(),
            msg,
        });
        let op = Op::Delete {
            key,
            ranking,
            at: 0,
        };
        self.ops.insert(id, (self.now, op));
        id
    }

    pub fn receive(&mut self, from: &str, msg: Msg<V>) {
        match msg {
            Msg::Get { op, key, have } => {
                let entry = self.home.get(&key).filter(|_| self.serves(&key));
                let hit = entry.map(|(value, version)| {
                    if have.as_ref() == Some(version) {
                        Hit::Same
                    } else {
                        Hit::Value(value.clone(), version.clone())
                    }
                });
                if hit.is_some() {
                    let until = self.now + self.timing.lease_wait_ms();
                    let lease = self.leases.entry(key).or_default();
                    *lease = (*lease).max(until);
                }
                let msg = Msg::Got { op, hit };
                self.sends.push(Send {
                    to: from.to_owned(),
                    msg,
                });
            }
            Msg::Got { op, hit } => {
                if !matches!(self.ops.get(&op), Some((_, Op::Fetch { .. }))) {
                    return;
                }
                let Some((
                    _,
                    Op::Fetch {
                        key,
                        sent,
                        have,
                        joined,
                    },
                )) = self.ops.remove(&op)
                else {
                    return;
                };
                let mut until = sent + self.timing.lease_ms;
                let value = match hit {
                    Some(Hit::Value(value, version)) => {
                        let near = Near {
                            value: value.clone(),
                            version,
                            until,
                        };
                        self.near.insert(key, near);
                        Some(value)
                    }
                    Some(Hit::Same) => match self.near.get_mut(&key) {
                        Some(near) if have.as_ref() == Some(&near.version) => {
                            near.until = near.until.max(until);
                            until = near.until;
                            Some(near.value.clone())
                        }
                        _ => None,
                    },
                    None => {
                        self.near.remove(&key);
                        None
                    }
                };
                self.fetched(op, joined, value.as_ref(), until);
            }
            Msg::Put { op, key, value } => {
                let reply = self.apply(&key, Some(value));
                self.reply(from, op, &key, reply);
            }
            Msg::Delete { op, key } => {
                let reply = self.apply(&key, None);
                self.reply(from, op, &key, reply);
            }
            Msg::Wrote { op, reply } => {
                let done = match self.ops.get(&op) {
                    Some((_, Op::Put { home })) if home == from => {
                        Done::Put(Some(matches!(reply, Reply::Applied(_))))
                    }
                    Some((_, Op::Delete { ranking, at, .. })) if ranking[*at] == from => {
                        match reply {
                            Reply::Applied(had) => Done::Delete(Some(had)),
                            Reply::NotHome => Done::Delete(Some(false)),
                            Reply::Inactive => return self.advance(op),
                        }
                    }
                    _ => return,
                };
                self.ops.remove(&op);
                self.done.push((op, done));
            }
        }
    }

    /// Ends fetch `id`, and the matches that joined it, with `value`, leased
    /// until `until`.
    fn fetched(&mut self, id: u64, joined: Vec<u64>, value: Option<&V>, until: u64) {
        for id in std::iter::once(id).chain(joined) {
            self.matched(id, value.cloned(), until);
        }
    }

    fn id(&mut self) -> u64 {
        self.next += 1;
        self.next
    }

    fn serves(&self, key: &str) -> bool {
        // An inactive node holds nothing.
        self.home_of(key) == Some(&self.me)
    }

    fn apply(&mut self, key: &str, value: Option<V>) -> Reply {
        if !self.active {
            return Reply::Inactive;
        }
        if !self.serves(key) {
            return Reply::NotHome;
        }
        let had = match value {
            Some(value) => {
                let version = (self.me.clone(), self.id());
                self.home.insert(key.to_owned(), (value, version)).is_some()
            }
            None => self.home.remove(key).is_some(),
        };
        Reply::Applied(had)
    }

    /// Answers a write once the leases this node granted on the key have run
    /// out: whatever it answers, it may have served the old value.
    fn reply(&mut self, from: &str, op: u64, key: &str, reply: Reply) {
        let msg = Msg::Wrote { op, reply };
        let send = Send {
            to: from.to_owned(),
            msg,
        };
        let at = self.leases.get(key).copied();
        match at.filter(|at| *at > self.now) {
            Some(at) => self.held.push((at, send)),
            None => self.sends.push(send),
        }
    }

    /// A write's target is not active or left the view: a put's outcome is
    /// unknown, and a delete goes on down the ranking.
    fn advance(&mut self, id: u64) {
        let Some((since, op)) = self.ops.remove(&id) else {
            return;
        };
        match op {
            Op::Fetch { .. } => {
                self.ops.insert(id, (since, op));
            }
            Op::Put { .. } => self.done.push((id, Done::Put(None))),
            Op::Delete { key, ranking, at } => {
                let view = &self.view;
                let Some(at) = (at + 1..ranking.len()).find(|&i| view.contains(&ranking[i])) else {
                    // No node in the ranking is active: nobody serves the key.
                    self.done.push((id, Done::Delete(Some(false))));
                    return;
                };
                let msg = Msg::Delete {
                    op: id,
                    key: key.clone(),
                };
                self.sends.push(Send {
                    to: ranking[at].clone(),
                    msg,
                });
                self.ops
                    .insert(id, (since, Op::Delete { key, ranking, at }));
            }
        }
    }
}

#[cfg(test)]
#[path = "cache-test.rs"]
mod tests;
