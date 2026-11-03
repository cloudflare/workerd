// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Adopting a new deployment catalog by restart.
//!
//! When a node that notices the catalog moved stops, so that its supervisor
//! starts it again on the new revision, and how far a roll has come.
//!
//! A node reads `deploy/catalog.json` every `POLL_MS` or so. A revision
//! other than the one it serves is checked first: its deployments must load
//! and compose with the node's own config, or a restart would only end in a
//! process that refuses to start. A node that refuses a revision says so in
//! its record and serves on; it checks again when the catalog moves.
//!
//! Nodes that accept a revision stop one at a time, by turns kept in one
//! object, `deploy/roll.json` (`Roll`), which they replace by conditional
//! writes only:
//!
//! - The first node to accept a revision writes it there, with the roll's
//!   baseline: how many nodes serve, or, while the previous roll is
//!   unfinished, its baseline if more. Once every live node serves a
//!   revision, as many as the baseline, a node that serves it marks the
//!   roll done.
//! - A node stops only once it has written itself in as the holder. It may
//!   while no holder is live and as many nodes serve as the baseline, so the
//!   holder before it is back, as a new node, since a node's id is new each
//!   start. A node that never comes back holds the roll there: until a
//!   supervisor starts it, the rest serve on, on the old revision. Deleting
//!   `roll.json`, or lowering its baseline, lets the roll go on without it.
//! - Its last read of the catalog succeeded, and its listing of the cluster
//!   is fresh: taken since it accepted the revision, at most `FRESH_MS` old,
//!   and with its own record naming the revision it serves. No live node may
//!   serve nothing yet, so a node that is starting counts once it serves.
//! - No live node failed to start on the revision (`Node::failed`). The
//!   roll then holds for good on every node that saw it; publishing a fix,
//!   or the previous version, is the way on.
//!
//! The holder reads the catalog once more before it stops, and gives the
//! turn back if the revision moved. A revision published after that read is
//! loaded unchecked by the restart; a node that then fails to start on it
//! says so, as above.

use crate::lease;

/// A node reads the catalog this often, scaled by `poll_delay_ms`.
pub const POLL_MS: u64 = 5_000;
/// A node waiting for its turn looks at its listing of the cluster this often.
pub const TURN_MS: u64 = 1_000;
/// A node waiting for its turn says why this often.
pub const REPORT_MS: u64 = 60_000;
/// The oldest listing of the cluster a node takes its turn by: two rounds.
pub const FRESH_MS: u64 = 2 * lease::INTERVAL_MS;

/// The wait before the first read, from 0 to `POLL_MS`, so that nodes
/// started together do not read together. `random` is any value.
#[must_use]
pub const fn first_delay_ms(random: u32) -> u64 {
    (random as u64 * POLL_MS) >> 32
}

/// The wait before each later read, from 3/4 to 5/4 of `POLL_MS`.
#[must_use]
pub const fn poll_delay_ms(random: u32) -> u64 {
    POLL_MS * 3 / 4 + ((random as u64 * (POLL_MS / 2)) >> 32)
}

/// What a read of the catalog found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Found {
    Unreadable,
    /// No catalog: no deployments, as before the first deploy.
    Absent,
    Revision(u64),
}

/// What the node does after a read.
#[derive(Debug, PartialEq, Eq)]
pub enum Polled {
    /// Nothing new: the revision the node serves, or one it already refused
    /// or accepted.
    Unchanged,
    /// The read failed; `first` for the first failure in a row.
    Unreadable { first: bool },
    /// The catalog that was there is gone; `first` for the first read that
    /// finds it so. A restart would serve no deployments, so the node serves
    /// on.
    Absent { first: bool },
    /// A revision to check, then report with `checked`.
    Check(u64),
}

/// How a revision's check came out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Checked {
    /// The deployments load and compose: restart on it in turn.
    Accept,
    /// They do not compose: serve on.
    Refuse,
    /// They do not load as stored (an object missing, or not what its
    /// version names), which a bad read can also look like: refused when the
    /// next check finds the same.
    Corrupt,
    /// The check could not finish (the bucket failed): check again at the
    /// next read.
    Retry,
}

/// A live node, as its record in the bucket describes it.
#[derive(Clone, Copy, Debug)]
pub struct Node<'a> {
    pub node: &'a str,
    /// The catalog revision it serves, 0 for none; `None` until it serves.
    pub revision: Option<u64>,
    /// The latest revision it refused.
    pub refused: Option<u64>,
    /// The revision it failed to start on.
    pub failed: Option<u64>,
}

impl Node<'_> {
    /// Whether it serves `revision`. Only that one: a catalog's revision
    /// can go down, by hand or when the catalog is made anew.
    #[must_use]
    pub fn serves(&self, revision: u64) -> bool {
        self.revision == Some(revision)
    }
}

/// `deploy/roll.json`: the roll to a revision, and whose turn it is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Roll {
    pub revision: u64,
    /// How many nodes must serve before the next may stop.
    pub baseline: usize,
    /// The node whose turn it is, until it is no longer live.
    pub holder: Option<String>,
    /// When the holder took the turn.
    pub since_ms: u64,
    /// Every live node serves the revision, as many as the baseline: the
    /// next roll counts the nodes that serve anew.
    pub done: bool,
}

/// What the node does about its turn.
#[derive(Debug, PartialEq, Eq)]
pub enum Turn {
    /// No revision to adopt.
    Idle,
    /// The last read of the catalog failed.
    Unconfirmed,
    /// The listing of the cluster is too old, or does not show this node
    /// serving.
    Stale,
    /// `node` is live but serves nothing yet.
    Starting(String),
    /// `node` failed to start on the revision: the roll holds.
    Failed(String),
    /// Write `roll` (conditionally, as every write of it): the roll to its
    /// revision begins. `kept` is `(serving, baseline)` when the baseline is
    /// the previous roll's, which more than serve.
    Begin {
        roll: Roll,
        kept: Option<(usize, usize)>,
    },
    /// `node` has the turn and is live.
    Held(String),
    /// Fewer nodes serve than the baseline: one that stopped is not back.
    Short { serving: usize, baseline: usize },
    /// Write `roll`, which names this node the holder, then read the catalog
    /// once more: stop if it still names `roll.revision`.
    Take(Roll),
    /// This node holds a turn it no longer takes: write `roll`.
    Release(Roll),
    /// The roll to the revision this node serves is complete: write `roll`,
    /// which says so.
    Finish(Roll),
}

/// One node's watch of the catalog.
#[derive(Debug)]
pub struct Watch {
    /// The revision this node serves, 0 for no catalog.
    serving: u64,
    /// The latest revision this node accepted, and when, until one that
    /// came later is refused or the catalog goes.
    accepted: Option<(u64, u64)>,
    /// The latest revision this node refused.
    refused: Option<u64>,
    /// The revision whose last check found it `Corrupt`.
    corrupt: Option<u64>,
    /// A revision a node failed to start on, and that node.
    failed: Option<(u64, String)>,
    failing: bool,
    absent: bool,
}

impl Watch {
    #[must_use]
    pub const fn new(serving: u64) -> Self {
        Self {
            serving,
            accepted: None,
            refused: None,
            corrupt: None,
            failed: None,
            failing: false,
            absent: false,
        }
    }

    /// The latest revision this node refused.
    #[must_use]
    pub const fn refused(&self) -> Option<u64> {
        self.refused
    }

    /// The revision this node accepted and waits for its turn on.
    #[must_use]
    pub fn accepted(&self) -> Option<u64> {
        self.accepted.map(|(revision, _)| revision)
    }

    /// Whether reads failed until this one, which succeeded.
    #[must_use]
    pub const fn failing(&self) -> bool {
        self.failing
    }

    /// A read of the catalog found `found`.
    pub fn polled(&mut self, found: Found) -> Polled {
        if found == Found::Unreadable {
            let first = !self.failing;
            self.failing = true;
            return Polled::Unreadable { first };
        }
        self.failing = false;
        let revision = match found {
            Found::Revision(revision) => revision,
            Found::Absent if self.serving == 0 => {
                self.accepted = None;
                return Polled::Unchanged;
            }
            Found::Absent | Found::Unreadable => {
                self.accepted = None;
                let first = !self.absent;
                self.absent = true;
                return Polled::Absent { first };
            }
        };
        self.absent = false;
        if revision == self.serving {
            // Back to what this node serves, by hand: nothing to adopt.
            self.accepted = None;
            return Polled::Unchanged;
        }
        if self.accepted() == Some(revision) {
            return Polled::Unchanged;
        }
        // A restart would load this revision, whatever was accepted before.
        self.accepted = None;
        if self.refused == Some(revision) {
            Polled::Unchanged
        } else {
            Polled::Check(revision)
        }
    }

    /// The check of `revision`, which `polled` asked for, at `now_ms`.
    /// Answers what to act on: `Corrupt` is `Retry` the first time.
    pub fn checked(&mut self, revision: u64, checked: Checked, now_ms: u64) -> Checked {
        let checked = match checked {
            Checked::Corrupt if self.corrupt != Some(revision) => {
                self.corrupt = Some(revision);
                return Checked::Retry;
            }
            Checked::Corrupt => Checked::Refuse,
            checked => checked,
        };
        self.corrupt = None;
        match checked {
            Checked::Accept => self.accepted = Some((revision, now_ms)),
            Checked::Refuse => {
                self.accepted = None;
                self.refused = Some(revision);
            }
            Checked::Corrupt | Checked::Retry => {}
        }
        checked
    }

    /// The holder read the catalog before it stopped, and it moved: the
    /// next read checks it.
    pub const fn moved(&mut self) {
        self.accepted = None;
    }

    /// What this node, `me`, does at `now_ms` about its turn, given the live
    /// nodes as its latest listing of the cluster, taken at `listed_ms`,
    /// describes them, and `roll`, as last read.
    pub fn turn(
        &mut self,
        me: &str,
        now_ms: u64,
        (listed_ms, live): (Option<u64>, &[Node<'_>]),
        roll: Option<&Roll>,
    ) -> Turn {
        if let Some(roll) = roll.filter(|roll| roll.holder.as_deref() == Some(me)) {
            return Turn::Release(Roll {
                holder: None,
                ..roll.clone()
            });
        }
        let Some((revision, accepted_ms)) = self.accepted else {
            return self.finish(me, now_ms, (listed_ms, live), roll);
        };
        if self.failing || self.absent {
            return Turn::Unconfirmed;
        }
        if let Some(node) = live.iter().find(|n| n.failed == Some(revision)) {
            self.failed = Some((revision, node.node.to_owned()));
        }
        if let Some((failed, node)) = &self.failed
            && *failed == revision
        {
            return Turn::Failed(node.clone());
        }
        let fresh = listed_ms.is_some_and(|at| at >= accepted_ms && now_ms <= at + FRESH_MS);
        if !fresh || !live.iter().any(|n| n.node == me && n.revision.is_some()) {
            return Turn::Stale;
        }
        if let Some(node) = live.iter().find(|n| n.revision.is_none()) {
            return Turn::Starting(node.node.to_owned());
        }
        let serving = live.len();
        let is_live = |node: &str| live.iter().any(|n| n.node == node);
        let Some(roll) = roll.filter(|roll| roll.revision == revision && !roll.done) else {
            // An unfinished roll keeps its baseline: its holder is not back
            // yet, or the cluster shrank before it was done.
            let roll = roll.filter(|roll| !roll.done);
            let kept = roll
                .filter(|roll| roll.baseline > serving)
                .map(|roll| (serving, roll.baseline));
            return Turn::Begin {
                roll: Roll {
                    revision,
                    baseline: kept.map_or(serving, |(_, baseline)| baseline),
                    holder: roll.and_then(|roll| roll.holder.clone()),
                    since_ms: roll.map_or(0, |roll| roll.since_ms),
                    done: false,
                },
                kept,
            };
        };
        if let Some(holder) = roll.holder.as_deref().filter(|holder| is_live(holder)) {
            return Turn::Held(holder.to_owned());
        }
        if serving < roll.baseline {
            return Turn::Short {
                serving,
                baseline: roll.baseline,
            };
        }
        Turn::Take(Roll {
            holder: Some(me.to_owned()),
            since_ms: now_ms,
            ..roll.clone()
        })
    }

    /// `Finish` when, by a fresh listing, the roll to the revision this node
    /// serves is complete: no holder is live, and every live node serves it,
    /// as many as its baseline. `Idle` otherwise.
    fn finish(
        &self,
        me: &str,
        now_ms: u64,
        (listed_ms, live): (Option<u64>, &[Node<'_>]),
        roll: Option<&Roll>,
    ) -> Turn {
        let Some(roll) = roll.filter(|roll| !roll.done && roll.revision == self.serving) else {
            return Turn::Idle;
        };
        let fresh = listed_ms.is_some_and(|at| now_ms <= at + FRESH_MS);
        let is_live = |node: &str| live.iter().any(|n| n.node == node);
        let complete = fresh
            && is_live(me)
            && !roll.holder.as_deref().is_some_and(is_live)
            && live.iter().all(|n| n.serves(roll.revision))
            && live.len() >= roll.baseline;
        if !complete {
            return Turn::Idle;
        }
        Turn::Finish(Roll {
            holder: None,
            done: true,
            ..roll.clone()
        })
    }
}

/// How far a roll to a revision has come, for a deploy that waits for it.
#[derive(Debug, PartialEq, Eq)]
pub struct Progress {
    /// Live nodes that serve the revision.
    pub done: usize,
    /// Live nodes that serve anything.
    pub serving: usize,
    /// Live nodes, those that do not serve yet included.
    pub live: usize,
    /// Live nodes that refused the revision.
    pub refused: Vec<String>,
    /// Live nodes that failed to start on it.
    pub failed: Vec<String>,
}

impl Progress {
    #[must_use]
    pub fn of(live: &[Node<'_>], revision: u64) -> Self {
        let named = |pick: fn(&Node<'_>) -> Option<u64>| {
            let mut nodes: Vec<String> = live
                .iter()
                .filter(|n| pick(n) == Some(revision) && !n.serves(revision))
                .map(|n| n.node.to_owned())
                .collect();
            nodes.sort();
            nodes
        };
        Self {
            done: live.iter().filter(|n| n.serves(revision)).count(),
            serving: live.iter().filter(|n| n.revision.is_some()).count(),
            live: live.len(),
            refused: named(|n| n.refused),
            failed: named(|n| n.failed),
        }
    }

    /// Whether the roll is done: every live node serves the revision, and
    /// as many as `baseline`, the nodes that served when the wait began, so
    /// that none of them is down for want of a replacement. A cluster with
    /// no live node is not done.
    #[must_use]
    pub const fn done(&self, baseline: usize) -> bool {
        self.live > 0 && self.done == self.live && self.done >= baseline
    }
}

/// What a read of the catalog found, for a deploy that waits for its roll.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CatalogSeen {
    Unreadable,
    Absent,
    /// Its revision, and whether it names the version the deploy published.
    Revision {
        revision: u64,
        names_version: bool,
    },
}

/// What a read of `deploy/roll.json` found, for a deploy that waits. One that does not parse is
/// `Absent`, as a node takes it.
#[derive(Clone, Copy, Debug)]
pub enum RollSeen<'a> {
    Unreadable,
    Absent,
    Present(&'a Roll),
}

/// What a deploy that waits does after a round of reads: each with the line to say.
#[derive(Debug, PartialEq, Eq)]
pub enum Waited {
    /// The roll is done.
    Done(String),
    /// It will not be done: the deploy fails.
    Fail(String),
    /// Not done yet: read again.
    Waiting(String),
}

/// A deploy's wait for the roll to the catalog revision that published its version, a round of
/// reads at a time (`step`).
///
/// The roll is done once every live node serves the revision, as many as served when the deploy
/// counted them (`baseline`) and as the roll's own baseline, and a node has marked the roll done
/// in `deploy/roll.json`; or, when no roll there names the revision, as when the nodes started on
/// it or the roll was deleted, once they serve it. A later revision is waited for instead while
/// it still names the version, since it rolls the version out too; one that does not supersedes
/// it. A node that refused the revision, or failed to start on it, fails the wait: neither will
/// serve it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Wait {
    /// The revision the deploy published, or found naming its version.
    pub published: u64,
    /// The revision waited for: `published`, or a later one that still names the version.
    pub revision: u64,
    /// How many live nodes served before the deploy published.
    pub baseline: usize,
}

impl Wait {
    #[must_use]
    pub const fn new(published: u64, baseline: usize) -> Self {
        Self {
            published,
            revision: published,
            baseline,
        }
    }

    /// What a round of reads means: of the catalog, of `deploy/roll.json`, and of the live nodes'
    /// records, `None` when they did not read.
    pub fn step(
        &mut self,
        catalog: CatalogSeen,
        roll: RollSeen<'_>,
        live: Option<&[Node<'_>]>,
    ) -> Waited {
        match catalog {
            // Unread, the catalog may name another version: the nodes' progress means nothing.
            CatalogSeen::Unreadable => {
                return Waited::Waiting("could not read deploy/catalog.json".to_owned());
            }
            CatalogSeen::Absent => {
                return Waited::Fail("deploy/catalog.json is missing".to_owned());
            }
            CatalogSeen::Revision { revision, .. } if revision == self.revision => {}
            CatalogSeen::Revision {
                revision,
                names_version: true,
            } => self.revision = revision,
            CatalogSeen::Revision { revision, .. } => {
                return Waited::Fail(format!(
                    "catalog revision {revision} does not select this deployment version"
                ));
            }
        }
        let Some(live) = live else {
            return Waited::Waiting("could not read the node records".to_owned());
        };
        let at = self.label();
        let progress = Progress::of(live, self.revision);
        if !progress.refused.is_empty() {
            return Waited::Fail(format!(
                "cannot complete the rollout to {at}. These nodes refused it: {}. Their configs \
                 cannot serve it. Check their logs",
                progress.refused.join(", "),
            ));
        }
        if !progress.failed.is_empty() {
            return Waited::Fail(format!(
                "cannot complete the rollout to {at}. These nodes failed to start: {}. Publish a \
                 fix or the previous version",
                progress.failed.join(", ")
            ));
        }
        // `None`: no roll names the revision. The nodes count those that served before, the
        // baseline of a roll they left unfinished included.
        let (baseline, marked) = match roll {
            RollSeen::Unreadable => (self.baseline, Some(false)),
            RollSeen::Present(roll) if roll.revision == self.revision => {
                (roll.baseline.max(self.baseline), Some(roll.done))
            }
            RollSeen::Present(_) | RollSeen::Absent => (self.baseline, None),
        };
        if progress.done(baseline) {
            return match (roll, marked) {
                (_, Some(true) | None) => Waited::Done(if progress.live == 1 {
                    format!("the only live node serves {at}")
                } else {
                    format!("all {} live nodes serve {at}", progress.live)
                }),
                (RollSeen::Unreadable, _) => Waited::Waiting(format!(
                    "all live nodes serve {at}, but deploy/roll.json could not be read"
                )),
                _ => Waited::Waiting(format!(
                    "all live nodes serve {at}. Waiting for deploy/roll.json to mark the rollout \
                     complete"
                )),
            };
        }
        let mut progressed = format!(
            "{} of {} live nodes serve {at}",
            progress.done, progress.live
        );
        if progress.serving < baseline {
            let missing = baseline - progress.serving;
            progressed +=
                &format!(". Waiting for {missing} more of the {baseline} nodes that served before");
        }
        Waited::Waiting(progressed)
    }

    /// The revision waited for, as the lines name it.
    fn label(&self) -> String {
        if self.revision == self.published {
            format!("catalog revision {}", self.revision)
        } else {
            format!(
                "catalog revision {}, a later deploy that also changed other deployments",
                self.revision
            )
        }
    }
}

#[cfg(test)]
#[path = "rollout-test.rs"]
mod tests;
