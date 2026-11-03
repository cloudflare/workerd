// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Cluster mode's deployments once the server serves.
//!
//! The instance's record names the catalog revision `serve` loaded, and a watch of the catalog
//! (`cluster::rollout`) stops the instance as SIGTERM does when another revision is to be
//! served, for its supervisor to start it again on that one. The process then exits with
//! `RESTART_STATUS`. An instance whose config fails with the deployments merged says so in its
//! record before it exits (`record_failure`), which holds the roll.

use std::cell::Cell;
use std::future::Future;
use std::rc::Rc;
use std::time::Duration;

use futures::future::Either;
use tokio::sync::Notify;

use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::Server;

/// The exit status of an instance that stopped for a restart on a new catalog revision.
///
/// It is `EX_TEMPFAIL`, a failure to retry, and not 0, so that a supervisor that restarts only a
/// process that failed (systemd's `Restart=on-failure`, Docker's `--restart on-failure`)
/// restarts it too; Kubernetes restarts any container that exits.
pub const RESTART_STATUS: i32 = 75;

/// How long the connections of an instance that stops for a restart may take to finish. No
/// supervisor ends a drain that the instance started itself; a WebSocket could hold it forever.
pub const RESTART_DRAIN: Duration = Duration::from_secs(30);

/// The deployments `serve` merged into the config before the server started.
pub struct Deployments {
    /// The catalog's revision, 0 when the bucket had none.
    pub revision: u64,
    /// The node's own config, before the deployments were merged into it: what a new revision
    /// is checked against.
    pub node_config: Vec<u8>,
}

/// Why the server stops.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reason {
    /// SIGTERM.
    Drain,
    /// A restart on a new catalog revision.
    Restart,
}

/// The server's stop (`run::run`'s `drain`), requested once, for the first reason. Clones share
/// it.
#[derive(Clone, Default)]
pub struct Stop(Rc<StopState>);

#[derive(Default)]
struct StopState {
    reason: Cell<Option<Reason>>,
    notify: Notify,
}

impl Stop {
    /// Requests the stop, unless it was already requested.
    pub fn request(&self, reason: Reason) {
        if self.0.reason.get().is_none() {
            self.0.reason.set(Some(reason));
            self.0.notify.notify_waiters();
        }
    }

    pub fn reason(&self) -> Option<Reason> {
        self.0.reason.get()
    }

    /// Resolves once the stop is requested, to why.
    pub async fn requested(&self) -> Reason {
        loop {
            let notified = self.0.notify.notified();
            if let Some(reason) = self.reason() {
                return reason;
            }
            notified.await;
        }
    }
}

/// Resolves on `sigterm` or once `stop` is requested, whichever comes first, which `stop` then
/// says: both drain the server the same way, a restart for at most `RESTART_DRAIN`.
pub async fn drain_signal(sigterm: impl Future<Output = ()>, stop: Stop) -> Option<Duration> {
    let sigterm = std::pin::pin!(sigterm);
    let requested = std::pin::pin!(stop.requested());
    if let Either::Left(((), _)) = futures::future::select(sigterm, requested).await {
        stop.request(Reason::Drain);
    }
    // A restart requested as the signal came stays one.
    (stop.reason() == Some(Reason::Restart)).then_some(RESTART_DRAIN)
}

/// Starts the catalog watch of an instance that serves, and names the revision it serves in its
/// record.
///
/// When another revision is to be served, `stop` is requested; the watch ends once the server
/// stops for any reason.
pub fn start(factory: &Factory, node: &Rc<ClusterNode>, deployments: Deployments, stop: Stop) {
    let cluster = Rc::clone(&node.cluster);
    cluster.serve_revision(deployments.revision);
    factory.spawn_detached(async move {
        let check = |loaded: &[cluster::deploy::Deployment]| {
            let is_set = |variable: &str| std::env::var_os(variable).is_some();
            cluster::compose::merge(&deployments.node_config, loaded, &is_set)
                .map(drop)
                .map_err(|e| e.to_string())
        };
        let watch = std::pin::pin!(cluster.watch_catalog(deployments.revision, &check));
        let stopped = std::pin::pin!(stop.requested());
        if let Either::Left(_) = futures::future::select(watch, stopped).await {
            stop.request(Reason::Restart);
        }
    });
}

/// Whether the node `server` runs failed to start on the catalog revision of `deployments`, which
/// it then records in its record, so that the roll to that revision holds.
///
/// Any error reported with the deployments merged in fails the revision, even one `--watch` would
/// serve with. The node is to exit then, `--watch` or not: only another catalog revision fixes
/// it, and a node that serves no revision holds every roll.
pub async fn record_failure(server: &Server, deployments: Option<&Deployments>) -> bool {
    let revision = deployments.map_or(0, |d| d.revision);
    let Some(node) = server.cluster() else {
        return false;
    };
    if revision == 0 || !server.report().had_errors() {
        return false;
    }
    tracing::error!(
        "cluster: this node cannot start on deployment catalog revision {revision}; the roll to \
         it holds until a fix, or the previous version, is published"
    );
    if let Err(e) = node.cluster.record_failure(revision).await {
        tracing::error!("cluster: recording the failure in this node's record: {e}");
    }
    true
}

#[cfg(test)]
#[path = "rollout-test.rs"]
mod tests;
