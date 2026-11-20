// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A deterministic simulation of the cache over a cluster: nodes with
//! drifting clocks register, renew and list leases in a bucket, crash,
//! leave, restart, lose the bucket and each other, while clients match, put
//! and delete two keys on every node. Messages are delayed, reordered and
//! dropped. The checker holds the history to the module's safety rule.
//!
//! Each clock is off the bucket's by a skew, about which it wobbles. Nodes
//! judge records by their own clocks, and join and fence as `Lease` does,
//! on the `Skew` of their answers' dates. Each skew is one that `Skew`
//! reads as within `max_skew_ms`. With `CACHE_DST_BLIND`, half are past it,
//! as far as one answer at the node's phase cannot tell from within it:
//! `Skew` must keep those out. With `CACHE_DST_BLIND=edge`, each is at one
//! end or the other of its range.
//! `CACHE_DST_STALLS` leaves only stalled writes and lost messages, so
//! that homes live long enough for the lease's margin to decide.

use std::collections::HashMap;

use super::*;
use crate::lease;

const L: lease::Timing = lease::Timing::DEFAULT;
const LEASE: u64 = L.lease_ms;
/// `Timing::safety_ms`: a node fences this long before its lease expires.
/// `CACHE_DST_SAFETY` overrides it.
const SAFETY: u64 = L.safety_ms;
const RENEW: u64 = L.interval_ms;
const LIST: u64 = L.interval_ms;
/// The bucket's clock at the simulation's start.
const BASE: u64 = 1_000_000;
/// How far a clock strays from its skew before it turns back.
const WOBBLE: i64 = 50;
const T: Timing = Timing::DEFAULT;
const RUN: u64 = 240_000;
/// After this, no faults or lost messages. Once the last cut bucket is
/// back, dead nodes' leases are out and nodes that registered again have
/// joined, every write must succeed.
const QUIET: u64 = 120_000;
const SETTLED: u64 = QUIET + 30_000 + LEASE + T.join_ms + 5_000;
const KEYS: [&str; 2] = ["a", "b"];

struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        Self(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1)
    }

    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }

    fn chance(&mut self, per_mille: u64) -> bool {
        self.below(1000) < per_mille
    }
}

struct Record {
    since: u64,
    /// By the writer's clock.
    expires: u64,
}

struct Node {
    id: String,
    cache: Cache<u64>,
    alive: bool,
    /// Local ms per 1,000 real ms, from the real and local times `at`.
    rate: u64,
    at: (u64, u64),
    /// The clock less the bucket's, give or take `WOBBLE`.
    skew: i64,
    /// Where in the bucket's second the node's requests are stamped: one
    /// place, as for rounds that recur at whole seconds, `Skew`'s worst.
    phase: u64,
    /// The `Skew` of the answers so far.
    estimate: Option<lease::Skew>,
    registering: Option<u64>,
    registered: Option<u64>,
    /// `Lease::until_ms`.
    until: Option<u64>,
    listed: Option<u64>,
    listing: Option<(u64, u64, Option<u64>, Vec<String>)>,
    view: Vec<String>,
    /// Draining, it serves nothing, and leaves at this time by its clock.
    leave: Option<u64>,
    next_renew: u64,
    next_list: u64,
    ops: HashMap<u64, usize>,
}

impl Node {
    fn new(id: &str, rng: &mut Rng, t: u64) -> Self {
        // Within 1% of its own time, as `Skew` allows.
        let rate = 991 + rng.below(19);
        let phase = rng.below(1000);
        // A skew `Lease::clock` passes at this phase: an answer at once puts
        // it in `skew + phase - 1000..=skew + phase`. Wobbling, and widened
        // by drift until the next renewal, it stays so. `Keeper::know_clock`
        // passes it too, with a lease's drift more, once answers at other
        // phases have narrowed it.
        let (max, p) = (L.max_skew_ms.cast_signed(), phase.cast_signed());
        let slack = WOBBLE + (RENEW / 100).cast_signed() + 10;
        let most = max - (L.round_ms() / 100).cast_signed() - slack;
        let join = most - (LEASE / 100).cast_signed();
        let blind = std::env::var("CACHE_DST_BLIND").ok();
        let (lo, hi) = if blind.is_some() && rng.chance(500) {
            // Past `max_skew_ms`, as far as an answer at this phase can
            // leave it in doubt: ahead early in the second, behind late.
            if p < 500 {
                (max + WOBBLE + 1, max + 1000 - p - WOBBLE - 1)
            } else {
                (-max - p + WOBBLE + 1, -max - WOBBLE - 1)
            }
        } else {
            ((-most + 1000 - p).max(-join), (most - p).min(join))
        };
        let skew = match blind.as_deref() {
            Some("edge") if rng.chance(500) => lo,
            Some("edge") => hi,
            _ => lo + rng.below((hi - lo).cast_unsigned()).cast_signed(),
        };
        let mut node = Self {
            id: id.to_owned(),
            cache: Cache::new(id, T),
            alive: true,
            rate,
            at: (t, (BASE + t).checked_add_signed(skew).unwrap()),
            skew,
            phase,
            estimate: None,
            registering: None,
            registered: None,
            until: None,
            listed: None,
            listing: None,
            view: Vec::new(),
            leave: None,
            next_renew: 0,
            next_list: 0,
            ops: HashMap::new(),
        };
        node.next_renew = node.local(t);
        node.next_list = node.local(t);
        node
    }

    fn local(&self, t: u64) -> u64 {
        self.at.1 + (t - self.at.0) * self.rate / 1000
    }

    /// Turns the clock back toward its skew once it strays past `WOBBLE`.
    fn wobble(&mut self, t: u64) {
        let off = self.local(t).cast_signed() - (BASE + t).cast_signed() - self.skew;
        if (off > WOBBLE && self.rate > 1000) || (off < -WOBBLE && self.rate < 1000) {
            self.at = (t, self.local(t));
            self.rate = 2000 - self.rate;
        }
    }

    /// A response to a request sent and answered at those local times,
    /// dated `date` by the bucket. The clock, never stepped, is its own
    /// monotonic clock.
    fn answered(&mut self, sent: u64, date: u64, received: u64) {
        let [sent, date, received] = [sent, date, received].map(u64::cast_signed);
        self.estimate = Some(lease::Skew::answered(self.estimate, sent, received, date));
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    Match,
    Put(u64),
    Delete,
}

struct Op {
    kind: Kind,
    key: &'static str,
    issued: u64,
    done: Option<(u64, Done<u64>)>,
}

struct Sim {
    rng: Rng,
    t: u64,
    nodes: Vec<Node>,
    incarnations: Vec<u64>,
    records: HashMap<String, Record>,
    net: Vec<(u64, String, String, Msg<u64>)>,
    cut: HashMap<String, u64>,
    /// Nodes whose writes the bucket stalls, though it answers their reads.
    stalled: HashMap<String, u64>,
    parted: Vec<(String, String, u64)>,
    /// When the transport tells a node of a dropped message: (at, node, op).
    losses: Vec<(u64, String, u64)>,
    safety: u64,
    stalls: bool,
    /// No message takes over 100 ms: the near copies of a test that counts
    /// fetches then outlive the fetches' answers.
    prompt: bool,
    history: Vec<Op>,
    values: u64,
    remote_hits: u64,
    /// Matches that joined a fetch in flight.
    joined: u64,
    /// Ops that `Cache::lost` finished.
    cut_short: u64,
    /// `CACHE_DST_TRACE=<from ms>`: print messages and ops after that.
    trace: u64,
}

impl Sim {
    fn new(seed: u64, n: usize) -> Self {
        let mut rng = Rng::new(seed);
        let nodes = (0..n)
            .map(|i| Node::new(&format!("n{i}.0"), &mut rng, 0))
            .collect();
        Self {
            rng,
            t: 0,
            nodes,
            incarnations: vec![0; n],
            records: HashMap::new(),
            net: Vec::new(),
            cut: HashMap::new(),
            stalled: HashMap::new(),
            parted: Vec::new(),
            losses: Vec::new(),
            safety: std::env::var("CACHE_DST_SAFETY")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(SAFETY),
            stalls: std::env::var_os("CACHE_DST_STALLS").is_some(),
            prompt: false,
            history: Vec::new(),
            values: 0,
            remote_hits: 0,
            joined: 0,
            cut_short: 0,
            trace: std::env::var("CACHE_DST_TRACE")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(u64::MAX),
        }
    }

    fn bucket_ok(&self, id: &str) -> bool {
        self.cut.get(id).is_none_or(|until| *until <= self.t)
    }

    fn writes_ok(&self, id: &str) -> bool {
        self.bucket_ok(id) && self.stalled.get(id).is_none_or(|until| *until <= self.t)
    }

    fn parted(&self, a: &str, b: &str) -> bool {
        self.parted
            .iter()
            .any(|(x, y, until)| *until > self.t && ((x == a && y == b) || (x == b && y == a)))
    }

    /// Registration, renewal, listing and fencing, as the lease executor
    /// does them.
    fn leases(&mut self, i: usize) {
        let t = self.t;
        let ok = self.bucket_ok(&self.nodes[i].id);
        let writes = self.writes_ok(&self.nodes[i].id);
        let node = &mut self.nodes[i];
        node.wobble(t);
        let now = node.local(t);
        // A clean stop, as `run.rs` makes it: at `Deadlines::leave` its
        // record expires at once.
        if node.leave.is_some_and(|at| at <= now) {
            self.records.remove(&node.id);
            node.alive = false;
            return;
        }
        if let Some(at) = node.registering
            && at <= t
        {
            node.registering = None;
            if ok {
                node.registered = Some(now);
            }
        }
        // `Lease::clock`, which `Keeper::know_clock` passes with a lease's
        // drift to spare before the node joins.
        let local = now.cast_signed();
        let ahead = if node.until.is_none() { LEASE / 100 } else { 0 };
        let skew_ms = node.estimate.map(|s| s.most_ms(local, local) + ahead);
        let known = lease::Lease::new(L).clock(skew_ms).is_ok();
        // `know_clock` gives up after a lease: the process exits, and comes
        // back, as another node, once its clock is fixed. Until the node
        // joins, `next_renew` is when it started.
        if node.until.is_none() && !known && now >= node.next_renew + LEASE {
            node.alive = false;
            return;
        }
        if now >= node.next_renew && node.registering.is_none() && (known || node.until.is_some()) {
            node.next_renew = now + RENEW;
            // `Lease::before_renew`: a clock not known well enough, or a
            // lease about to lapse, ends the process, which restarts as
            // another node. The record it leaves expires in its time.
            let lapsing = |u| now + L.renew_timeout_ms + self.safety >= u;
            if node.until.is_some_and(lapsing) || !known {
                self.restart(i);
                return;
            }
            match (writes, self.records.get_mut(&node.id)) {
                // A stalled renewal fails at its round's bound, which is as
                // long as `maintain` goes between checks.
                (false, _) => node.next_renew = now + L.renew_timeout_ms,
                (true, Some(record)) => {
                    record.expires = now + LEASE;
                    node.until = Some(now + LEASE);
                    node.answered(now, BASE + t - node.phase, now);
                    // A registration whose answer was lost: later is safe.
                    node.registered.get_or_insert(now);
                }
                // Never written: register, which takes a while to apply and
                // longer to return.
                (true, None) => {
                    let apply = t + self.rng.below(300);
                    let back = apply + self.rng.below(300);
                    self.records.insert(
                        node.id.clone(),
                        Record {
                            since: apply,
                            expires: now + LEASE,
                        },
                    );
                    node.until = Some(now + LEASE);
                    node.registered = None;
                    node.registering = Some(back);
                }
            }
        }
        if now >= node.next_list && node.listing.is_none() && ok {
            // `Keeper::know_clock` reads more often, so not at whole seconds,
            // and its reads answer in their usual time (32 ms at the median).
            // Only its reads and the renewals measure the clock: the loop's
            // listings go through a client of their own.
            let joining = node.until.is_none() && !known;
            node.next_list = now + if joining { L.retry_ms / 4 } else { LIST };
            let mut view: Vec<String> = self
                .records
                .iter()
                .filter(|(_, r)| r.since <= t && r.expires > now)
                .map(|(id, _)| id.clone())
                .collect();
            view.sort();
            let back = t + 5 + self.rng.below(if joining { 50 } else { 1_500 });
            let stamp = t + self.rng.below(back - t + 1);
            let date = joining.then(|| BASE + stamp - self.rng.below(1000));
            node.listing = Some((back, now, date, view));
        }
        if let Some((back, ..)) = &node.listing
            && *back <= t
        {
            let (_, start, date, view) = node.listing.take().unwrap();
            if ok {
                if let Some(date) = date {
                    node.answered(start, date, now);
                }
                node.listed = Some(start);
                node.view = view;
            }
        }
        // `Timing::view`: `!holds(now + lease_wait_ms)`.
        let wait = T.lease_wait_ms() + self.safety;
        let fenced = node.until.is_none_or(|u| now + wait >= u);
        let fresh = !fenced && T.fresh(now, node.listed);
        let joined = T.active(now, node.listed, node.registered);
        let active = fresh && joined && node.leave.is_none();
        node.cache.set(now, node.view.clone(), fresh, active);
        node.cache.tick(now);
    }

    fn index(&self, id: &str) -> Option<usize> {
        self.nodes.iter().position(|n| n.alive && n.id == id)
    }

    fn flush(&mut self, i: usize) {
        let from = self.nodes[i].id.clone();
        for send in self.nodes[i].cache.take_sends() {
            let lost = send.to != from
                && self.t < QUIET
                && (self.rng.chance(20) || self.parted(&from, &send.to));
            if lost {
                // Sometimes the transport knows, a while later, and tells
                // the node that sent the request.
                let (asker, op) = match &send.msg {
                    Msg::Get { op, .. } | Msg::Put { op, .. } | Msg::Delete { op, .. } => {
                        (&from, *op)
                    }
                    Msg::Got { op, .. } | Msg::Wrote { op, .. } => (&send.to, *op),
                };
                if self.rng.chance(500) {
                    let at = self.t + self.rng.below(3_000);
                    self.losses.push((at, asker.clone(), op));
                }
                continue;
            }
            let delay = if send.to == from {
                0
            } else if !self.prompt && self.rng.chance(50) {
                100 + self.rng.below(3_000)
            } else {
                1 + self.rng.below(100)
            };
            self.net
                .push((self.t + delay, from.clone(), send.to, send.msg));
        }
        for (op, done) in self.nodes[i].cache.take_done() {
            let Some(h) = self.nodes[i].ops.remove(&op) else {
                continue;
            };
            if self.t >= self.trace {
                eprintln!(
                    "{} {from} done {:?} {:?}",
                    self.t, self.history[h].kind, done
                );
            }
            self.history[h].done = Some((self.t, done));
        }
    }

    fn deliver(&mut self) {
        for _ in 0..1_000 {
            let t = self.t;
            let (mut due, later) = std::mem::take(&mut self.net)
                .into_iter()
                .partition::<Vec<_>, _>(|m| m.0 <= t);
            self.net = later;
            if due.is_empty() {
                return;
            }
            // Shuffle: due messages arrive in any order.
            for k in (1..due.len()).rev() {
                due.swap(k, self.rng.below(k as u64 + 1) as usize);
            }
            for (_, from, to, msg) in due {
                let Some(i) = self.index(&to) else {
                    continue;
                };
                if self.t >= self.trace {
                    eprintln!("{} {from} -> {to} {msg:?}", self.t);
                }
                if matches!(msg, Msg::Got { hit: Some(_), .. }) {
                    self.remote_hits += 1;
                }
                self.nodes[i].cache.receive(&from, msg);
                self.flush(i);
            }
        }
        panic!("messages never settle");
    }

    /// Tells nodes of the dropped messages whose time came (`Cache::lost`).
    fn losses(&mut self) {
        let t = self.t;
        let (due, later) = std::mem::take(&mut self.losses)
            .into_iter()
            .partition::<Vec<_>, _>(|l| l.0 <= t);
        self.losses = later;
        for (_, id, op) in due {
            if let Some(i) = self.index(&id) {
                if self.t >= self.trace {
                    eprintln!("{} {id} lost op {op}", self.t);
                }
                let open = self.nodes[i].ops.contains_key(&op);
                self.nodes[i].cache.lost(op);
                self.flush(i);
                if open && !self.nodes[i].ops.contains_key(&op) {
                    self.cut_short += 1;
                }
            }
        }
    }

    fn client(&mut self) {
        let alive: Vec<usize> = (0..self.nodes.len())
            .filter(|&i| self.nodes[i].alive)
            .collect();
        // Every clock may be too far off to join.
        if alive.is_empty() {
            return;
        }
        let i = alive[self.rng.below(alive.len() as u64) as usize];
        let key = KEYS[self.rng.below(KEYS.len() as u64) as usize];
        let roll = self.rng.below(10);
        let kind = if roll < 5 {
            Kind::Match
        } else if roll < 8 {
            self.values += 1;
            Kind::Put(self.values)
        } else {
            Kind::Delete
        };
        let cache = &mut self.nodes[i].cache;
        let op = match kind {
            Kind::Match => cache.get(key),
            Kind::Put(v) => cache.put(key, v),
            Kind::Delete => cache.delete(key),
        };
        let joined = |(_, o): &(u64, super::Op)| matches!(o, super::Op::Fetch { joined, .. } if joined.contains(&op));
        if cache.ops.values().any(joined) {
            self.joined += 1;
        }
        if self.t >= self.trace {
            let id = &self.nodes[i].id;
            eprintln!("{} {id} issue {kind:?} {key} op {op}", self.t);
        }
        self.nodes[i].ops.insert(op, self.history.len());
        self.history.push(Op {
            kind,
            key,
            issued: self.t,
            done: None,
        });
        self.flush(i);
    }

    fn faults(&mut self) {
        let n = self.nodes.len();
        let i = self.rng.below(n as u64) as usize;
        let alive = self.nodes.iter().filter(|n| n.alive && n.leave.is_none());
        let alive = alive.count();
        let (crash, leave, cut) = if self.stalls { (0, 0, 0) } else { (2, 20, 2) };
        if self.rng.chance(crash) && self.nodes[i].alive && alive > 1 {
            // A crash: its ops never finish, its record lingers.
            self.nodes[i].alive = false;
        } else if self.rng.chance(leave)
            && self.nodes[i].alive
            && !self.nodes[i].cache.leases.is_empty()
            && alive > 1
        {
            // A clean stop that matters, with copies out: the node expires
            // its record once they have run out.
            let at = self.nodes[i].local(self.t) + T.lease_wait_ms();
            self.nodes[i].leave.get_or_insert(at);
        } else if self.rng.chance(10) && !self.nodes[i].alive {
            self.restart(i);
        } else if self.rng.chance(cut) {
            let until = self.t + 1_000 + self.rng.below(30_000);
            self.cut.insert(self.nodes[i].id.clone(), until);
        } else if self.rng.chance(2) {
            // Renewals fail and listings succeed: the lease fences.
            let until = self.t + 1_000 + self.rng.below(30_000);
            self.stalled.insert(self.nodes[i].id.clone(), until);
        } else if self.rng.chance(3) {
            let j = self.rng.below(n as u64) as usize;
            let until = self.t + 500 + self.rng.below(10_000);
            let pair = (self.nodes[i].id.clone(), self.nodes[j].id.clone());
            self.parted.push((pair.0, pair.1, until));
        }
    }

    fn restart(&mut self, i: usize) {
        self.incarnations[i] += 1;
        let id = format!("n{i}.{}", self.incarnations[i]);
        self.nodes[i] = Node::new(&id, &mut self.rng, self.t);
    }

    fn run(&mut self, until: u64) {
        while self.t < until {
            self.t += 1 + self.rng.below(20);
            if self.t < QUIET {
                self.faults();
            }
            for i in 0..self.nodes.len() {
                if self.nodes[i].alive {
                    self.leases(i);
                    self.flush(i);
                }
            }
            self.deliver();
            self.losses();
            if self.rng.chance(300) {
                self.client();
            }
        }
    }

    fn check(&self, seed: u64) {
        let put_of: HashMap<u64, &Op> = self
            .history
            .iter()
            .filter_map(|op| match op.kind {
                Kind::Put(v) => Some((v, op)),
                _ => None,
            })
            .collect();
        // Writes that took effect: (key, issued, done).
        let writes: Vec<(&str, u64, u64)> = self
            .history
            .iter()
            .filter_map(|op| match op.done {
                Some((at, Done::Put(Some(true)) | Done::Delete(Some(_)))) => {
                    Some((op.key, op.issued, at))
                }
                _ => None,
            })
            .collect();
        for m in &self.history {
            let Some((_, Done::Match(Some(v)))) = m.done else {
                continue;
            };
            let p = put_of[&v];
            assert_eq!(p.key, m.key, "seed {seed}");
            let stored = match p.done {
                Some((at, Done::Put(Some(true)))) => Some(at),
                Some((_, Done::Put(Some(false)))) => {
                    panic!("seed {seed}: match returned {v}, which was never stored")
                }
                _ => None,
            };
            // A put that never said it stored is concurrent with all after.
            let Some(stored) = stored else { continue };
            for &(key, issued, done) in &writes {
                assert!(
                    !(key == m.key && issued > stored && done < m.issued),
                    "seed {seed}: match at {} returned {v} (stored {stored}), \
                     overwritten by a write issued {issued} done {done}",
                    m.issued
                );
            }
        }
        for op in &self.history {
            let late = op.issued + T.timeout_ms > RUN;
            if op.issued < SETTLED || late || op.kind == Kind::Match {
                continue;
            }
            let ok = matches!(
                op.done,
                Some((_, Done::Put(Some(true)) | Done::Delete(Some(_))))
            );
            assert!(
                ok,
                "seed {seed}: write {:?} at {} failed when quiet: {:?}",
                op.kind, op.issued, op.done
            );
        }
    }
}

#[test]
fn timing_keeps_one_node_serving_a_key() {
    // Real ms for `ms` on the slowest and the fastest clock.
    let (slow, fast) = (|ms: u64| ms * 1_000 / 990, |ms: u64| ms * 1_000 / 1_010);
    // A joined node is in every fresh view: `join_ms` on the fastest clock
    // outlasts a view's staleness on the slowest, with the listing's 1.5 s
    // and registration's 0.6 s.
    assert!(fast(T.join_ms) > slow(T.stale_ms) + 2_100);
    // A node fences before its record expires.
    assert!(fast(LEASE) > slow(LEASE - SAFETY));
    // A home waits out the longest a copy may be served.
    assert!(fast(T.lease_wait_ms()) > slow(T.lease_ms));
    // A fenced home's copies run out before its record expires, so before
    // the next home serves the key.
    assert!(LEASE - slow(LEASE - SAFETY) > slow(T.lease_ms));
    // An old home lists a joining one, or goes stale, and stops serving the
    // key; its copies run out before the new home is active. Whatever the
    // listings' cadence, which is `maintain`'s in the code and `LIST` here.
    assert!(fast(T.join_ms) > slow(T.stale_ms) + 2_100 + slow(T.lease_wait_ms()));
}

impl Sim {
    /// Runs without faults or clients for `ms`.
    fn quiet(&mut self, ms: u64) {
        let end = self.t + ms;
        while self.t < end {
            self.t += 10;
            for i in 0..self.nodes.len() {
                if self.nodes[i].alive {
                    self.leases(i);
                    self.flush(i);
                }
            }
            self.deliver();
            self.losses();
        }
    }

    /// `kind` on `key` at node `i`, run until done.
    fn op(&mut self, i: usize, kind: Kind, key: &'static str) -> Done<u64> {
        let cache = &mut self.nodes[i].cache;
        let op = match kind {
            Kind::Match => cache.get(key),
            Kind::Put(v) => cache.put(key, v),
            Kind::Delete => cache.delete(key),
        };
        let h = self.history.len();
        self.nodes[i].ops.insert(op, h);
        let issued = self.t;
        self.history.push(Op {
            kind,
            key,
            issued,
            done: None,
        });
        self.flush(i);
        self.deliver();
        while self.history[h].done.is_none() {
            self.quiet(10);
        }
        self.history[h].done.clone().unwrap().1
    }
}

#[test]
fn a_write_invalidates_every_near_copy() {
    let mut sim = Sim::new(1, 3);
    // Past `QUIET`, where no message is lost.
    sim.quiet(QUIET);
    sim.prompt = true;
    let home = sim.nodes[0].cache.home_of("a").unwrap().to_owned();
    let w = sim.nodes.iter().position(|n| n.id != home).unwrap();
    assert_eq!(sim.op(w, Kind::Put(7), "a"), Done::Put(Some(true)));
    for i in 0..3 {
        assert_eq!(sim.op(i, Kind::Match, "a"), Done::Match(Some(7)));
    }
    // Now near copies: no message needed.
    let fetched = sim.remote_hits;
    for i in 0..3 {
        assert_eq!(sim.op(i, Kind::Match, "a"), Done::Match(Some(7)));
    }
    assert_eq!(sim.remote_hits, fetched);
    let d = (w + 1) % 3;
    assert_eq!(sim.op(d, Kind::Delete, "a"), Done::Delete(Some(true)));
    for i in 0..3 {
        assert_eq!(sim.op(i, Kind::Match, "a"), Done::Match(None));
    }
    assert_eq!(sim.op(d, Kind::Delete, "a"), Done::Delete(Some(false)));
}

#[test]
fn a_near_copy_goes_ten_leases_after_its_own() {
    let mut sim = Sim::new(1, 3);
    sim.quiet(QUIET);
    let home = sim.nodes[0].cache.home_of("a").unwrap().to_owned();
    let w = sim.nodes.iter().position(|n| n.id != home).unwrap();
    assert_eq!(sim.op(w, Kind::Put(7), "a"), Done::Put(Some(true)));
    assert_eq!(sim.op(w, Kind::Match, "a"), Done::Match(Some(7)));
    // Past its lease it renews by version, without the value.
    sim.quiet(2 * T.lease_ms);
    let fetched = sim.remote_hits;
    assert_eq!(sim.op(w, Kind::Match, "a"), Done::Match(Some(7)));
    assert!(sim.nodes[w].cache.near.contains_key("a"));
    sim.quiet(11 * T.lease_ms);
    assert!(!sim.nodes[w].cache.near.contains_key("a"));
    assert_eq!(sim.op(w, Kind::Match, "a"), Done::Match(Some(7)));
    assert!(sim.remote_hits > fetched);
}

#[test]
fn a_node_drops_what_its_view_moves_away() {
    let mut sim = Sim::new(2, 3);
    sim.quiet(40_000);
    let old = sim.nodes[0].cache.home_of("a").unwrap().to_owned();
    let h = sim.nodes.iter().position(|n| n.id == old).unwrap();
    assert_eq!(sim.op(h, Kind::Put(1), "a"), Done::Put(Some(true)));
    // A node joins that outranks every other for "a", stores a value there
    // and dies; "a" goes back to its old home, which must not serve 1.
    let ids: Vec<String> = sim.nodes.iter().map(|n| n.id.clone()).collect();
    let top = |id: &String| ids.iter().all(|o| weight("a", id) > weight("a", o));
    let id = (0..10_000).map(|i| format!("j{i}")).find(top).unwrap();
    let t = sim.t;
    let node = Node::new(&id, &mut sim.rng, t);
    sim.nodes.push(node);
    sim.incarnations.push(0);
    sim.quiet(40_000);
    let j = sim.nodes.len() - 1;
    assert_eq!(sim.nodes[h].cache.home_of("a"), Some(id.as_str()));
    assert_eq!(sim.op(j, Kind::Put(2), "a"), Done::Put(Some(true)));
    sim.nodes[j].alive = false;
    sim.quiet(40_000);
    assert_eq!(sim.nodes[h].cache.home_of("a"), Some(old.as_str()));
    assert_eq!(sim.op(h, Kind::Match, "a"), Done::Match(None));
}

#[test]
fn a_lost_request_ends_a_match_or_put_at_once() {
    let mut a = Cache::new("a", T);
    let view = vec!["a".to_owned(), "b".to_owned()];
    a.set(0, view.clone(), true, true);
    let key = (0..64)
        .map(|i| format!("k{i}"))
        .find(|k| a.home_of(k) == Some("b"))
        .unwrap();
    let get = a.get(&key);
    let hit = Some(Hit::Value(1, ("b".to_owned(), 1)));
    a.receive("b", Msg::Got { op: get, hit });
    assert_eq!(a.take_done(), [(get, Done::Match(Some(1)))]);
    // The near copy ran out, and its renewal is lost: no value.
    a.set(T.lease_ms, view, true, true);
    let get = a.get(&key);
    a.lost(get);
    assert_eq!(a.take_done(), [(get, Done::Match(None))]);
    // Its late answer is ignored.
    let hit = Some(Hit::Same);
    a.receive("b", Msg::Got { op: get, hit });
    assert!(a.take_done().is_empty());
    let put = a.put(&key, 2);
    a.lost(put);
    assert_eq!(a.take_done(), [(put, Done::Put(None))]);
    // A delete waits for its answer, or for its target to leave the view.
    let delete = a.delete(&key);
    a.lost(delete);
    assert!(a.take_done().is_empty());
    let reply = Reply::Applied(true);
    a.receive("b", Msg::Wrote { op: delete, reply });
    assert_eq!(a.take_done(), [(delete, Done::Delete(Some(true)))]);
}

#[test]
fn simulation_holds_the_safety_rule() {
    let var = |name, or| {
        std::env::var(name)
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(or)
    };
    // `CACHE_DST_FROM` shards a long run.
    let (from, seeds): (u64, u64) = (var("CACHE_DST_FROM", 0), var("CACHE_DST_SEEDS", 30));
    let mut remote_hits = 0;
    let mut joined = 0;
    let mut cut_short = 0;
    let mut matches = 0;
    for seed in from..from + seeds {
        let mut sim = Sim::new(seed, 3 + (seed % 3) as usize);
        sim.run(RUN);
        sim.check(seed);
        remote_hits += sim.remote_hits;
        joined += sim.joined;
        cut_short += sim.cut_short;
        matches += sim
            .history
            .iter()
            .filter(|op| matches!(op.done, Some((_, Done::Match(Some(_))))))
            .count();
    }
    // Not vacuous: entries are fetched from homes and hit, and lost
    // messages end ops early.
    assert!(remote_hits > seeds, "{remote_hits}");
    assert!(joined > seeds, "{joined}");
    assert!(cut_short > seeds, "{cut_short}");
    assert!(matches > (seeds * 50) as usize, "{matches}");
}

/// `CACHE_DST_SEED=<seed> CACHE_DST_AT=<ms>`, with `CACHE_DST_TRACE`:
/// replays a seed and prints each node's lease state at that time.
#[test]
fn replay_seed() {
    let var = |name| std::env::var(name).ok().and_then(|s| s.parse().ok());
    let (Some(seed), Some(at)) = (var("CACHE_DST_SEED"), var("CACHE_DST_AT")) else {
        return;
    };
    let mut sim = Sim::new(seed, 3 + (seed % 3) as usize);
    sim.run(at);
    for n in &sim.nodes {
        eprintln!(
            "{} alive={} skew={} estimate={:?} now={} registered={:?} \
             until={:?} listed={:?} view={:?} record={:?} cut={:?}",
            n.id,
            n.alive,
            n.skew,
            n.estimate,
            n.local(sim.t),
            n.registered,
            n.until,
            n.listed,
            n.view,
            sim.records.get(&n.id).map(|r| (r.since, r.expires)),
            sim.cut.get(&n.id),
        );
    }
}
