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
//! - **Links.** Two loop threads talk over a session of their own ([`ThreadLink`]
//!   (crate::threads::ThreadLink)): the hub makes the pipe it runs over the first time either
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

use tokio::sync::mpsc;

use crate::placement::Placement;
use crate::placement::Scope;
use crate::threads::SessionEnd;
use crate::threads::session_pipe;

/// What a loop thread is sent through its inbox.
pub enum Mail {
    /// The hub has an end of a session with this thread for the thread to take
    /// ([`Hub::link`]): another thread asked for the link.
    Link(usize),
}

/// The hub of a process's loop threads. See the module.
pub struct Hub {
    placement: Placement,
    /// Each loop thread's inbox, by its index.
    inboxes: Vec<mpsc::UnboundedSender<Mail>>,
    /// The ends of the pipes made so far that their threads have not taken, by the pair of
    /// threads, lower index first: the lower thread's end, and the higher's.
    links: Mutex<HashMap<(usize, usize), (Option<SessionEnd>, Option<SessionEnd>)>>,
}

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
                drop(if to < from { ends.0.take() } else { ends.1.take() });
            }
            ends
        });
        if from < to {
            ends.0.take()
        } else {
            ends.1.take()
        }
    }

    /// Sends `mail` to thread `to`'s inbox; false if the thread is gone, or none of the
    /// process's.
    pub fn post(&self, to: usize, mail: Mail) -> bool {
        self.inboxes
            .get(to)
            .is_some_and(|inbox| inbox.send(mail).is_ok())
    }
}

#[cfg(test)]
#[path = "hub-test.rs"]
mod tests;
