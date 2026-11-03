// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Adopting a new deployment catalog by restart: the executor of
//! `cluster_logic::rollout`, which has the rules.
//!
//! A node reads the catalog every few seconds once it serves. On a revision
//! other than its own it loads the deployments and has the server check
//! them against its config; then it waits for its turn, by `ROLL` and the
//! records of the live nodes as its lease rounds list them, and answers the
//! revision for the server to stop on. Its supervisor starts it again, and
//! `serve` loads the catalog afresh.

use std::time::Duration;

use cluster_logic::rollout;
use cluster_logic::rollout::Checked;
use cluster_logic::rollout::Found;
use cluster_logic::rollout::Node;
use cluster_logic::rollout::Polled;
pub use cluster_logic::rollout::Progress;
use cluster_logic::rollout::Roll;
use cluster_logic::rollout::Turn;
use cluster_logic::rollout::Watch;
use ring::rand::SecureRandom;
use ring::rand::SystemRandom;
use serde::Deserialize;
use serde::Serialize;
use tokio::time::Instant;
use tokio::time::timeout;

use crate::Bucket;
use crate::Error;
use crate::Guard;
use crate::NodeRecord;
use crate::Registry;
use crate::Result;
use crate::Version;
use crate::deploy;
use crate::deploy::Deployment;
use crate::now_ms;

/// Whether this node's config can serve a revision's deployments: the
/// reason, one line or more, when it cannot.
pub type Check<'a> = dyn Fn(&[Deployment]) -> std::result::Result<(), String> + 'a;

/// The latest roll, and whose turn it is to restart.
///
/// `{"revision": <n>, "baseline": <nodes>, "holder": "<node>", "since_ms": <ms>, "done": <bool>}`.
/// Nodes replace it by conditional writes only. Deleting it, or lowering
/// `baseline`, lets a roll that waits for a node gone for good go on.
pub const ROLL: &str = "deploy/roll.json";

/// How long a read of the catalog, or a write of `ROLL`, may take.
const READ_TIMEOUT: Duration = Duration::from_secs(10);
/// How long reading a revision's deployments may take, each up to 16 MiB.
const LOAD_TIMEOUT: Duration = Duration::from_mins(2);

#[derive(Serialize, Deserialize)]
struct RollFile {
    revision: u64,
    baseline: usize,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    holder: Option<String>,
    #[serde(default)]
    since_ms: u64,
    #[serde(default)]
    done: bool,
}

/// `ROLL` as last read: what it says, `None` when it is absent or does not
/// parse, and its version, which the next write requires.
#[derive(Default)]
struct Cached {
    roll: Option<Roll>,
    version: Option<Version>,
}

/// Watches the catalog until this node, which serves revision `serving`
/// (0 for no catalog), is to stop for a restart on another: answers it.
pub(crate) async fn watch(
    bucket: &Bucket,
    registry: &Registry,
    serving: u64,
    check: &Check<'_>,
) -> u64 {
    let mut watch = Watch::new(serving);
    tokio::time::sleep(ms(rollout::first_delay_ms(random()))).await;
    let mut next_read = Instant::now();
    let mut roll = Cached::default();
    let mut first = true;
    let mut reported: Option<(Turn, Instant)> = None;
    loop {
        if Instant::now() >= next_read {
            read(bucket, registry, &mut watch, serving, check).await;
            next_read = Instant::now() + ms(rollout::poll_delay_ms(random()));
            // Read while this node rolls, holds the turn, or may finish the
            // roll to what it serves.
            let holder = roll.roll.as_ref().and_then(|r| r.holder.as_deref());
            let finishing = roll
                .roll
                .as_ref()
                .is_some_and(|r| !r.done && r.revision == serving);
            if first || finishing || watch.accepted().is_some() || holder == Some(registry.node()) {
                refresh(bucket, &mut roll).await;
            }
            first = false;
        }
        let (listed_ms, records) = registry.listing(now_ms());
        let live = nodes(&records);
        let me = registry.node();
        let turn = watch.turn(me, now_ms(), (listed_ms, &live), roll.roll.as_ref());
        match &turn {
            Turn::Idle => {
                reported = None;
                tokio::time::sleep_until(next_read).await;
                continue;
            }
            Turn::Begin { roll: next, kept } => {
                if let Some((serving, baseline)) = kept {
                    tracing::warn!(
                        "cluster: rollout of deployment catalog revision {} needs {baseline} \
                         serving nodes. {serving} are serving. This minimum comes from the \
                         unfinished rollout. If the cluster shrank, lower `baseline` in {ROLL} or \
                         delete the file",
                        next.revision
                    );
                }
                if write(bucket, &mut roll, next).await {
                    tracing::info!(
                        "cluster: rolling out deployment catalog revision {} one node at a time. \
                         The rollout requires {} serving nodes",
                        next.revision,
                        next.baseline
                    );
                }
                continue;
            }
            Turn::Release(next) => {
                write(bucket, &mut roll, next).await;
                continue;
            }
            Turn::Finish(next) => {
                if write(bucket, &mut roll, next).await {
                    tracing::info!(
                        "cluster: rollout of deployment catalog revision {} is complete",
                        next.revision
                    );
                }
                continue;
            }
            Turn::Take(next) => {
                if !write(bucket, &mut roll, next).await {
                    continue;
                }
                // The restart loads whatever the catalog names then.
                if confirmed(bucket, next.revision).await {
                    tracing::warn!(
                        "cluster: stopping to restart on deployment catalog revision {}",
                        next.revision
                    );
                    return next.revision;
                }
                tracing::info!(
                    "cluster: {} changed before this node stopped. Releasing its rollout turn",
                    deploy::CATALOG
                );
                watch.moved();
                next_read = Instant::now();
                continue;
            }
            _ => {}
        }
        // Said when the reason changes, and again every `REPORT_MS` while it holds.
        let due = reported
            .as_ref()
            .is_none_or(|(said, at)| *said != turn || at.elapsed() >= ms(rollout::REPORT_MS));
        if due {
            report(&turn, watch.accepted().unwrap_or_default());
            reported = Some((turn, Instant::now()));
        }
        let wake = next_read.min(Instant::now() + ms(rollout::TURN_MS));
        tokio::time::sleep_until(wake).await;
    }
}

fn report(turn: &Turn, revision: u64) {
    match turn {
        Turn::Unconfirmed => {
            tracing::info!("cluster: waiting to restart until the deployment catalog can be read");
        }
        Turn::Stale => {
            tracing::info!(
                "cluster: waiting to restart until a fresh cluster listing shows this node as \
                 serving"
            );
        }
        Turn::Starting(node) => tracing::info!(
            "cluster: waiting for node {node} to serve before restarting. Its record has no \
             catalog revision. It may still be starting, lack `--experimental`, or lack rollout \
             support. Any of these conditions blocks the rollout"
        ),
        Turn::Failed(node) => tracing::error!(
            "cluster: cannot restart on deployment catalog revision {revision}. Node {node} \
             failed to start on it. The rollout cannot continue. Publish a fix or the previous \
             version"
        ),
        Turn::Held(node) => {
            tracing::info!("cluster: waiting for node {node}, which holds the rollout turn");
        }
        Turn::Short { serving, baseline } => tracing::info!(
            "cluster: waiting for a stopped node to return before restarting. {serving} nodes \
             serve, but the rollout requires {baseline}. See {ROLL}"
        ),
        Turn::Idle | Turn::Begin { .. } | Turn::Take(_) | Turn::Release(_) | Turn::Finish(_) => {}
    }
}

/// Reads `ROLL` into `cached`, which stays as it was if the read fails.
async fn refresh(bucket: &Bucket, cached: &mut Cached) {
    let (body, version) = match timeout(READ_TIMEOUT, bucket.get(ROLL)).await {
        Ok(Ok(Some(found))) => found,
        Ok(Ok(None)) => {
            *cached = Cached::default();
            return;
        }
        Ok(Err(e)) => return tracing::debug!("cluster: reading {ROLL} failed: {e}"),
        Err(_) => return tracing::debug!("cluster: reading {ROLL} timed out"),
    };
    let roll = match serde_json::from_slice::<RollFile>(&body) {
        Ok(file) => Some(Roll {
            revision: file.revision,
            baseline: file.baseline,
            holder: file.holder,
            since_ms: file.since_ms,
            done: file.done,
        }),
        Err(e) => {
            tracing::warn!("cluster: cannot parse {ROLL}: {e}. The next rollout will replace it");
            None
        }
    };
    *cached = Cached {
        roll,
        version: Some(version),
    };
}

/// Writes `roll` over `ROLL` as `cached` last read it: whether it landed.
/// If not, another node wrote first or the bucket failed, and `cached` is
/// read again after a pause.
async fn write(bucket: &Bucket, cached: &mut Cached, roll: &Roll) -> bool {
    let file = RollFile {
        revision: roll.revision,
        baseline: roll.baseline,
        holder: roll.holder.clone(),
        since_ms: roll.since_ms,
        done: roll.done,
    };
    let body = serde_json::to_vec_pretty(&file).unwrap_or_default();
    let guard = cached.version.clone().map_or(Guard::Absent, Guard::Match);
    match timeout(READ_TIMEOUT, bucket.put_if(ROLL, body, guard)).await {
        Ok(Ok(Some(version))) => {
            *cached = Cached {
                roll: Some(roll.clone()),
                version: Some(version),
            };
            return true;
        }
        Ok(Ok(None)) => {}
        Ok(Err(e)) => tracing::warn!("cluster: writing {ROLL} failed: {e}"),
        Err(_) => tracing::warn!("cluster: writing {ROLL} timed out"),
    }
    tokio::time::sleep(ms(rollout::TURN_MS)).await;
    refresh(bucket, cached).await;
    false
}

/// Whether the catalog still names `revision`.
async fn confirmed(bucket: &Bucket, revision: u64) -> bool {
    let read = timeout(READ_TIMEOUT, deploy::catalog(bucket)).await;
    matches!(read, Ok(Ok(Some((catalog, _)))) if catalog.revision == revision)
}

/// One read of the catalog, and the check of a revision it finds.
async fn read(
    bucket: &Bucket,
    registry: &Registry,
    watch: &mut Watch,
    serving: u64,
    check: &Check<'_>,
) {
    let (found, error) = match timeout(READ_TIMEOUT, deploy::catalog(bucket)).await {
        Ok(Ok(Some((catalog, _)))) => (Found::Revision(catalog.revision), String::new()),
        Ok(Ok(None)) => (Found::Absent, String::new()),
        Ok(Err(e)) => (Found::Unreadable, e.to_string()),
        Err(_) => (Found::Unreadable, "timed out".to_owned()),
    };
    let failing = watch.failing();
    match watch.polled(found) {
        Polled::Unreadable { first: true } => tracing::warn!(
            "cluster: cannot read {}: {error}. Continuing to serve revision {serving}",
            deploy::CATALOG
        ),
        Polled::Absent { first: true } => tracing::error!(
            "cluster: {} is missing. Continuing to serve revision {serving} because restarting \
             would remove all deployments",
            deploy::CATALOG
        ),
        Polled::Check(revision) => {
            let (checked, reason) = check_revision(bucket, revision, check).await;
            match watch.checked(revision, checked, now_ms()) {
                Checked::Accept => tracing::info!(
                    "cluster: deployment catalog revision {revision} passed validation. Waiting \
                     for this node's restart turn"
                ),
                Checked::Refuse => {
                    tracing::error!(
                        "cluster: cannot restart on deployment catalog revision {revision}: \
                         {reason}. Continuing to serve revision {serving}"
                    );
                    registry.refuse(revision);
                }
                Checked::Corrupt | Checked::Retry if !reason.is_empty() => tracing::warn!(
                    "cluster: will check deployment catalog revision {revision} again after the \
                     next catalog read: {reason}"
                ),
                Checked::Corrupt | Checked::Retry => {}
            }
        }
        Polled::Unchanged | Polled::Unreadable { .. } | Polled::Absent { .. } => {}
    }
    if failing && !watch.failing() {
        tracing::info!("cluster: can read {} again", deploy::CATALOG);
    }
}

/// Whether this node can restart on catalog revision `revision`: its
/// deployments load, and compose with this node's config. With the reason
/// when they do not.
async fn check_revision(bucket: &Bucket, revision: u64, check: &Check<'_>) -> (Checked, String) {
    match timeout(LOAD_TIMEOUT, deploy::load(bucket)).await {
        // The catalog moved on meanwhile: the next read checks the latest.
        Ok(Ok(Some(loaded))) if loaded.revision != revision => (Checked::Retry, String::new()),
        Ok(Ok(Some(loaded))) => match check(&loaded.deployments) {
            Ok(()) => (Checked::Accept, String::new()),
            Err(reason) => (Checked::Refuse, reason),
        },
        Ok(Ok(None)) => (Checked::Retry, String::new()),
        Ok(Err(Error::Deploy(reason))) => (Checked::Corrupt, reason),
        Ok(Err(e)) => (Checked::Retry, format!("could not load deployments: {e}")),
        Err(_) => (Checked::Retry, "deployment load timed out".to_owned()),
    }
}

fn nodes(records: &[NodeRecord]) -> Vec<Node<'_>> {
    records.iter().map(node).collect()
}

fn node(record: &NodeRecord) -> Node<'_> {
    Node {
        node: &record.node,
        revision: record.revision,
        refused: record.refused,
        failed: record.failed,
    }
}

/// How far the live nodes `records` have come in a roll to `revision`.
pub fn progress(records: &[NodeRecord], revision: u64) -> Progress {
    Progress::of(&nodes(records), revision)
}

/// The records of the nodes live at `now_ms`, as the bucket holds them now.
/// Reads only, unlike a node's own listing (`Registry::peers`).
pub async fn live_nodes(bucket: &Bucket, now_ms: u64) -> Result<Vec<NodeRecord>> {
    let listed = bucket.list("nodes/").await?;
    // Not the `.probe` a joining node writes and deletes.
    let keys = listed
        .iter()
        .map(|entry| entry.key.as_str())
        .filter(|key| key.strip_suffix(".json").is_some());
    let read = keys.map(|key| async move {
        let Some((body, _)) = bucket.get(key).await? else {
            return Ok(None);
        };
        let record: NodeRecord = serde_json::from_slice(&body)?;
        Ok::<_, Error>((record.expires_ms > now_ms).then_some(record))
    });
    let mut live = Vec::new();
    for record in futures::future::join_all(read).await {
        live.extend(record?);
    }
    Ok(live)
}

/// Any value, for spreading reads out.
fn random() -> u32 {
    let mut bytes = [0; 4];
    // Without randomness every node reads on the same schedule, which works.
    let _ = SystemRandom::new().fill(&mut bytes);
    u32::from_le_bytes(bytes)
}

const fn ms(ms: u64) -> Duration {
    Duration::from_millis(ms)
}

#[cfg(test)]
#[path = "rollout-test.rs"]
mod tests;
