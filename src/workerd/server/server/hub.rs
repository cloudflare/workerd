// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The hub of the loop threads of `workerd serve --threads=<N>` (threads.rs): what decides where
//! a Durable Object lives, and what introduces two loop threads to each other.
//!
//! The hub is state the loop threads share, and runs on none of them: it is `Send + Sync`,
//! every call into it returns at once, and it has no task of its own, so a thread that is busy
//! running JavaScript delays nothing the hub does for the others. The process has one, made
//! with the threads and kept beside the front (`run::Threads`), which is made whenever there is
//! more than one loop thread.
//!
//! - **Placement.** [`Hub::home`] is the one answer to "which thread is this actor's home": a
//!   loop thread asks it for every stub it makes, and does not work a home out itself, so the
//!   policy ([`Placement`]: a fixed-key hash over the threads) can become a table without the
//!   threads knowing. The front can ask too, with what names the actor.
//! - **Links.** Two loop threads talk over a session of their own (`threads::ThreadLink`): the
//!   hub makes the pipe it runs over the first time either
//!   asks ([`Hub::link`]), keeps each thread's end until that thread takes it, and tells the
//!   other thread through its inbox. There is one pipe for a pair of threads, however many
//!   stubs cross it and whichever thread asked first, so the calls of one stub arrive in the
//!   order they were made. No byte of a session passes through the hub: once each thread has
//!   its end the two talk directly, and a call takes one hop. A link is made when it is first
//!   needed rather than at startup: making one needs nothing of any thread but the two it
//!   joins, so there is no moment at which every thread must be up for the mesh to exist, and
//!   a process whose actors never call across a given pair never pays for that pair.
//! - **Inboxes.** Each loop thread has one ([`Mail`]): plain messages that any thread, or the
//!   hub, sends it and that its loop takes in order (`Factory::serve_links` starts the task).
//!   What is sent before the thread's graph is built waits there.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::PoisonError;

use futures::future::BoxFuture;
use tokio::sync::mpsc;
use tokio::sync::oneshot;

use crate::placement::Placement;
use crate::placement::Scope;
use crate::threads::SessionEnd;
use crate::threads::session_pipe;

/// What a loop thread is sent through its inbox.
pub enum Mail {
    /// The hub has an end of a session with this thread for the thread to take
    /// ([`Hub::link`]): another thread asked for the link.
    Link(usize),
    /// Abort every evictable actor of this thread, with the reason if there is one
    /// (`abortAllDurableObjects()` on another thread).
    AbortAll(Option<Reason>, Done),
    /// Abort every evictable actor of this thread and delete its storage and alarms
    /// (`deleteAllDurableObjects()` on another thread).
    DeleteAll(Option<Reason>, Done),
    /// The eviction test hook for every running actor of this thread in the namespaces named,
    /// each by its worker service and class; `hibernate` as for the hook.
    EvictAll {
        namespaces: Vec<(String, String)>,
        hibernate: bool,
        done: Done,
    },
    /// The eviction test hook for the root actor `key` of the namespace of `service`'s `class`,
    /// whose home is this thread.
    Evict {
        service: String,
        class: String,
        key: String,
        hibernate: bool,
        done: Done,
    },
    /// In a cluster, for the first thread: thread `from` asks the control plane for an object
    /// whose home it is. `answer` is none for a call that has no answer (`Abandon`,
    /// `ForgetRoute`), which is in effect before the next mail is taken.
    Control {
        from: usize,
        call: ControlCall,
        answer: Option<oneshot::Sender<ControlAnswer>>,
    },
    /// In a cluster: schedule the alarm of the object `actor` of the namespace whose hash is
    /// `namespace`, whose home is this thread, adopted from a dead peer.
    AdoptAlarm {
        namespace: String,
        actor: String,
        name: Option<String>,
        at_ms: u64,
    },
    /// In a cluster: the instance drains. Hand off this thread's objects by `deadlines`, and
    /// answer whether every one was stopped.
    HandOff {
        deadlines: crate::cluster::Deadlines,
        stopped: oneshot::Sender<bool>,
    },
}

/// What a thread asks of the instance's control plane in a cluster (`cluster::Control`), which
/// is the first thread's: the call, by the names of `cluster::Control`'s methods.
pub enum ControlCall {
    Claim {
        ns: String,
        actor: String,
        now_ms: u64,
    },
    Advance {
        ns: String,
        actor: String,
        epoch: u64,
    },
    Open {
        ns: String,
        actor: String,
        epoch: u64,
    },
    Release {
        ns: String,
        actor: String,
        epoch: u64,
    },
    Abandon {
        ns: String,
        actor: String,
    },
    ForgetRoute {
        ns: String,
        actor: String,
        node: String,
    },
    RefreshRoute {
        ns: String,
        actor: String,
        now_ms: u64,
    },
    ArmAlarm {
        ns: String,
        actor: String,
        name: Option<String>,
        at_ms: Option<u64>,
    },
    OpenedAlarm {
        ns: String,
        actor: String,
        name: Option<String>,
        at_ms: u64,
    },
    AlarmElsewhere {
        ns: String,
        actor: String,
        owner: String,
        now_ms: u64,
    },
}

impl ControlCall {
    /// The namespace and the object the call is for.
    #[must_use]
    pub fn object(&self) -> (&str, &str) {
        match self {
            Self::Claim { ns, actor, .. }
            | Self::Advance { ns, actor, .. }
            | Self::Open { ns, actor, .. }
            | Self::Release { ns, actor, .. }
            | Self::Abandon { ns, actor }
            | Self::ForgetRoute { ns, actor, .. }
            | Self::RefreshRoute { ns, actor, .. }
            | Self::ArmAlarm { ns, actor, .. }
            | Self::OpenedAlarm { ns, actor, .. }
            | Self::AlarmElsewhere { ns, actor, .. } => (ns, actor),
        }
    }
}

impl ControlCall {
    /// The answer to this call that says it failed with `error`.
    #[must_use]
    pub fn failed(&self, error: cluster::Error) -> ControlAnswer {
        match self {
            Self::Claim { .. } => ControlAnswer::Claim(Err(error)),
            Self::Advance { .. } => ControlAnswer::Advance(Err(error)),
            Self::Open { .. } | Self::Release { .. } | Self::AlarmElsewhere { .. } => {
                ControlAnswer::Flag(Err(error))
            }
            Self::Abandon { .. }
            | Self::ForgetRoute { .. }
            | Self::RefreshRoute { .. }
            | Self::ArmAlarm { .. }
            | Self::OpenedAlarm { .. } => ControlAnswer::Done(Err(error)),
        }
    }
}

/// The control plane's answer to a [`ControlCall`].
pub enum ControlAnswer {
    Claim(cluster::Result<cluster::Claim>),
    Advance(cluster::Result<Option<cluster::Owned>>),
    /// Of `Open`, `Release` and `AlarmElsewhere`.
    Flag(cluster::Result<bool>),
    /// Of `RefreshRoute`, `ArmAlarm` and `OpenedAlarm`.
    Done(cluster::Result<()>),
}

/// Why actors are aborted: the exception's type and description.
pub type Reason = (cxx::KjExceptionType, String);

/// Where a thread says that it did what a mail asked, or why it could not: the description of
/// the error. Dropped unsent by a thread that ended first.
pub type Done = oneshot::Sender<Result<(), String>>;

/// The hub of a process's loop threads. See the module.
pub struct Hub {
    placement: Placement,
    /// Each loop thread's inbox, by its index.
    inboxes: Vec<mpsc::UnboundedSender<Mail>>,
    /// The ends of the pipes made so far that their threads have not taken, by the pair of
    /// threads, lower index first: the lower thread's end, and the higher's.
    links: Mutex<HashMap<(usize, usize), Ends>>,
}

/// The ends of a pipe that their threads have not taken: the lower thread's, and the higher's.
type Ends = (Option<SessionEnd>, Option<SessionEnd>);

/// A loop thread's seat among the process's: which thread it is, the hub, and its inbox. Made
/// with the hub, for the thread that takes it (`Factory::join`).
pub struct Seat {
    pub index: usize,
    pub hub: Arc<Hub>,
    pub inbox: mpsc::UnboundedReceiver<Mail>,
}

impl Hub {
    /// The hub of `placement.count` loop threads, and each thread's seat, in order.
    #[must_use]
    pub fn new(placement: Placement) -> (Arc<Self>, Vec<Seat>) {
        let (inboxes, received): (Vec<_>, Vec<_>) = (0..placement.count)
            .map(|_| mpsc::unbounded_channel())
            .unzip();
        let hub = Arc::new(Self {
            placement,
            inboxes,
            links: Mutex::default(),
        });
        let seats = received.into_iter().enumerate().map(|(index, inbox)| Seat {
            index,
            hub: Arc::clone(&hub),
            inbox,
        });
        (Arc::clone(&hub), seats.collect())
    }

    /// How many loop threads the process has.
    #[must_use]
    pub fn threads(&self) -> usize {
        self.placement.count
    }

    /// The home thread of the root actor `key` (the hex id of a durable actor, the name of an
    /// ephemeral one) of the namespace `scope` names; its facets live there too.
    #[must_use]
    pub fn home(&self, scope: &Scope<'_>, key: &str) -> usize {
        self.placement.home(scope, key)
    }

    /// Thread `from`'s end of the pipe its session with thread `to` runs over, made if neither
    /// asked before, in which case `to` is told through its inbox. None if `from` has taken its
    /// end already, or either is no thread of the process, or is both.
    #[must_use]
    pub fn link(&self, from: usize, to: usize) -> Option<SessionEnd> {
        if from == to || from.max(to) >= self.threads() {
            return None;
        }
        let pair = (from.min(to), from.max(to));
        let mut links = self.links.lock().unwrap_or_else(PoisonError::into_inner);
        let ends = links.entry(pair).or_insert_with(|| {
            let (lower, higher) = session_pipe();
            let mut ends = (Some(lower), Some(higher));
            // A thread that is gone takes no end: its end is dropped, which ends the session
            // for the other.
            if !self.post(to, Mail::Link(from)) {
                drop(if to < from {
                    ends.0.take()
                } else {
                    ends.1.take()
                });
            }
            ends
        });
        let end = if from < to {
            ends.0.take()
        } else {
            ends.1.take()
        };
        drop(links);
        end
    }

    /// Sends `mail` to thread `to`'s inbox; false if the thread is gone, or none of the
    /// process's.
    pub fn post(&self, to: usize, mail: Mail) -> bool {
        self.inboxes
            .get(to)
            .is_some_and(|inbox| inbox.send(mail).is_ok())
    }

    /// Sends every thread but `from` the mail `make` makes for it, and resolves once each has
    /// done what its mail asked: with the first error among them, or that of a thread that
    /// ended without an answer.
    pub fn broadcast(
        &self,
        from: usize,
        make: impl Fn(Done) -> Mail,
    ) -> BoxFuture<'static, Result<(), String>> {
        let answers: Vec<_> = (0..self.threads())
            .filter(|thread| *thread != from)
            .map(|thread| {
                let (done, answer) = oneshot::channel();
                // A thread that is gone drops the mail, and with it `done`.
                let _ = self.post(thread, make(done));
                (thread, answer)
            })
            .collect();
        Box::pin(async move {
            let mut result = Ok(());
            for (thread, answer) in answers {
                let answer = answer
                    .await
                    .unwrap_or_else(|_| Err(format!("loop thread {thread} has ended")));
                result = result.and(answer);
            }
            result
        })
    }
}

#[cfg(test)]
#[path = "hub-test.rs"]
mod tests;
