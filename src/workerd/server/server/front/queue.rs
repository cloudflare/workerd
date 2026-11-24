// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The queue the front puts work in and the loop threads take it from.
//!
//! One queue holds the work of every socket, first in, first out: a loop serves every socket the
//! same way, so queues per socket would only give a loop several things to wait for.
//!
//! The front never chooses who serves an item. A loop takes one when it has run out of other
//! things to do: its event loop calls [`Parking::before_park`] when neither KJ nor tokio has
//! anything ready on the thread (`kj_rs_tokio::ParkHooks`), and if the queue holds work then, the
//! loop is granted one take instead of sleeping. A loop that runs JavaScript does not get there,
//! so the work goes to the loops that do, and a loop takes its next item only after it has run
//! the last one as far as it goes without waiting. Nothing is taken on behalf of a loop, so
//! nothing can be stranded on a loop that is busy.
//!
//! A loop that is asleep cannot notice new work, so [`Queue::push`] wakes one: of the loops that
//! sleep, the one with the fewest items in flight, which keeps a loop that has many requests
//! suspended from collecting more while others have none; among equals the turn goes around.
//! It wakes at most that one loop per
//! item, and none if every loop is awake: each of those looks at the queue before it sleeps.
//! Either the push sees the loop asleep or the loop sees the item queued; both read after they
//! wrote (`SeqCst`), so not both miss.
//!
//! There is no cap on what a loop has in flight: a cap would leave work queued behind
//! long-lived items (WebSockets, long polls) with loops free to serve it.
//!
//! An item leaves the queue in one of three ways. A loop takes it ([`Worker::poll_take`]), which
//! is one step under the queue's lock: a loop that has stopped taking (a dropped [`Worker`])
//! never holds an item it will not serve, and what it leaves queued is for the others, one of
//! which it wakes. Its [`Ticket`] is dropped while it is queued, which is how the request of a
//! client that went away leaves. Or the last loop is gone, and the queue drops what it holds and
//! is given.

use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering::SeqCst;
use std::task::Context;
use std::task::Poll;

use futures::task::AtomicWaker;

/// The front's end of the queue.
pub struct Queue<T> {
    shared: Arc<Shared<T>>,
}

impl<T> Clone for Queue<T> {
    fn clone(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
        }
    }
}

struct Shared<T> {
    state: Mutex<State<T>>,
    /// How many items are queued, for a loop about to sleep to read without the lock.
    queued: AtomicUsize,
    /// Every item with a smaller id has been taken: a ticket for one has nothing to remove.
    taken_through: AtomicU64,
    /// The queue is closed and empty: the loops are done with it.
    ended: AtomicBool,
}

struct State<T> {
    /// The items with their ids, which ascend.
    items: VecDeque<(u64, T)>,
    next_id: u64,
    /// No more items will be pushed.
    closed: bool,
    /// There were loops, and the last is gone: nothing will be taken.
    deserted: bool,
    loops: Vec<Arc<LoopState>>,
    /// Where the search for a sleeping loop starts.
    next_sleeper: usize,
}

/// What the queue knows of one loop.
#[derive(Default)]
struct LoopState {
    /// The loop's thread sleeps, or is about to, and has not been woken for an item.
    asleep: AtomicBool,
    /// The loop may take one item.
    granted: AtomicBool,
    /// Items the loop took whose [`InFlight`] it still holds.
    in_flight: AtomicUsize,
    /// Wakes the task that takes for the loop.
    taker: AtomicWaker,
    /// The loop's [`Worker`] is gone.
    gone: AtomicBool,
}

impl LoopState {
    /// Lets the loop take one item, and wakes it to do so.
    fn grant(&self) {
        self.granted.store(true, SeqCst);
        self.taker.wake();
    }
}

impl<T> Shared<T> {
    fn lock(&self) -> MutexGuard<'_, State<T>> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Records the queue's length after a change under the lock, and that the loops are done
    /// if it is closed and empty; then they are woken to find out.
    fn changed(&self, state: &State<T>) {
        self.queued.store(state.items.len(), SeqCst);
        if state.closed && state.items.is_empty() && !self.ended.swap(true, SeqCst) {
            for worker in &state.loops {
                worker.taker.wake();
            }
        }
    }
}

impl<T> State<T> {
    /// Of the loops that sleep, the one with the fewest items in flight, no longer counted
    /// asleep: the caller wakes it. Among loops with equally few, the turn goes around, so that
    /// a server with little to do does not serve everything on one loop.
    fn claim_sleeper(&mut self) -> Option<Arc<LoopState>> {
        let count = self.loops.len();
        loop {
            let (index, sleeper) = (0..count)
                .map(|offset| (self.next_sleeper + offset) % count)
                .map(|index| (index, &self.loops[index]))
                .filter(|(_, worker)| worker.asleep.load(SeqCst))
                .min_by_key(|(_, worker)| worker.in_flight.load(SeqCst))?;
            // The loop may have woken by itself meanwhile; then another is chosen.
            if sleeper.asleep.swap(false, SeqCst) {
                let sleeper = Arc::clone(sleeper);
                self.next_sleeper = (index + 1) % count;
                return Some(sleeper);
            }
        }
    }
}

impl<T> Queue<T> {
    #[must_use]
    pub fn new() -> Self {
        Self {
            shared: Arc::new(Shared {
                state: Mutex::new(State {
                    items: VecDeque::new(),
                    next_id: 0,
                    closed: false,
                    deserted: false,
                    loops: Vec::new(),
                    next_sleeper: 0,
                }),
                queued: AtomicUsize::new(0),
                taken_through: AtomicU64::new(0),
                ended: AtomicBool::new(false),
            }),
        }
    }

    /// Queues `item` behind what is queued, and wakes a sleeping loop for it if there is one.
    ///
    /// The item stays queued for as long as the ticket is kept, or until a loop takes it. A
    /// queue that is closed or that the last loop has left drops the item instead.
    pub fn push(&self, item: T) -> Ticket<T> {
        let shared = &self.shared;
        let mut state = shared.lock();
        let id = state.next_id;
        state.next_id += 1;
        let ticket = Ticket {
            shared: Arc::clone(shared),
            id,
            removes: true,
        };
        if state.closed || state.deserted {
            drop(state);
            drop(item);
            return ticket;
        }
        state.items.push_back((id, item));
        shared.changed(&state);
        let sleeper = state.claim_sleeper();
        drop(state);
        if let Some(sleeper) = sleeper {
            sleeper.grant();
        }
        ticket
    }

    /// Says that nothing more will be pushed. The loops take what is queued, and are then done
    /// ([`Worker::poll_take`]).
    pub fn close(&self) {
        let mut state = self.shared.lock();
        state.closed = true;
        self.shared.changed(&state);
        drop(state);
    }

    /// A loop's end of the queue. The loop takes nothing before its [`Parking`] is told that
    /// it is about to sleep, or a push finds it asleep.
    #[must_use]
    pub fn worker(&self) -> Worker<T> {
        let me = Arc::new(LoopState::default());
        self.shared.lock().loops.push(Arc::clone(&me));
        Worker {
            shared: Arc::clone(&self.shared),
            me,
        }
    }

    /// How many items are queued.
    #[must_use]
    pub fn len(&self) -> usize {
        self.shared.queued.load(SeqCst)
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl<T> Default for Queue<T> {
    fn default() -> Self {
        Self::new()
    }
}

/// Keeps an item queued: dropping it removes the item if no loop has taken it.
pub struct Ticket<T> {
    shared: Arc<Shared<T>>,
    id: u64,
    /// Dropping the ticket removes the item.
    removes: bool,
}

impl<T> Ticket<T> {
    /// Leaves the item queued until a loop takes it, whatever becomes of who queued it.
    pub fn keep_queued(mut self) {
        self.removes = false;
    }
}

impl<T> Drop for Ticket<T> {
    fn drop(&mut self) {
        let shared = &self.shared;
        if !self.removes || self.id < shared.taken_through.load(SeqCst) {
            return;
        }
        let mut state = shared.lock();
        let Ok(index) = state.items.binary_search_by_key(&self.id, |(id, _)| *id) else {
            return;
        };
        let removed = state.items.remove(index);
        shared.changed(&state);
        drop(state);
        // The item may own a great deal; it is dropped without the lock.
        drop(removed);
    }
}

/// A loop's end of the queue. Dropping it is how a loop stops taking: what is queued stays
/// queued for the other loops, and when it was the last, the queue drops its items.
pub struct Worker<T> {
    shared: Arc<Shared<T>>,
    me: Arc<LoopState>,
}

impl<T> Worker<T> {
    /// The next item, if this loop may take one and one is queued; `None` once the queue is
    /// closed and empty. Otherwise the task is woken when the loop may take.
    ///
    /// Taking is one step: an item is either still queued, for any loop, or returned here, to
    /// a caller that is running. The [`InFlight`] counts the item as this loop's until it is
    /// dropped.
    pub fn poll_take(&self, cx: &mut Context<'_>) -> Poll<Option<(T, InFlight)>> {
        let shared = &self.shared;
        self.me.taker.register(cx.waker());
        if shared.ended.load(SeqCst) {
            return Poll::Ready(None);
        }
        if !self.me.granted.swap(false, SeqCst) {
            return Poll::Pending;
        }
        let mut state = shared.lock();
        let Some((id, item)) = state.items.pop_front() else {
            // Another loop was quicker, or the client went away.
            return Poll::Pending;
        };
        shared.taken_through.store(id + 1, SeqCst);
        self.me.in_flight.fetch_add(1, SeqCst);
        shared.changed(&state);
        drop(state);
        Poll::Ready(Some((item, InFlight(Arc::clone(&self.me)))))
    }

    /// What tells the queue when this loop's thread sleeps and wakes.
    #[must_use]
    pub fn parking(&self) -> Parking<T> {
        Parking {
            shared: Arc::clone(&self.shared),
            me: Arc::clone(&self.me),
        }
    }
}

impl<T> Drop for Worker<T> {
    fn drop(&mut self) {
        let shared = &self.shared;
        self.me.gone.store(true, SeqCst);
        self.me.asleep.store(false, SeqCst);
        let mut state = shared.lock();
        state.loops.retain(|worker| !Arc::ptr_eq(worker, &self.me));
        if state.loops.is_empty() {
            state.deserted = true;
            let abandoned = std::mem::take(&mut state.items);
            shared.changed(&state);
            drop(state);
            drop(abandoned);
            return;
        }
        // This loop may have been woken for an item it now leaves behind.
        let sleeper = if state.items.is_empty() {
            None
        } else {
            state.claim_sleeper()
        };
        drop(state);
        if let Some(sleeper) = sleeper {
            sleeper.grant();
        }
    }
}

/// Counts an item as in flight on the loop that took it, until dropped.
pub struct InFlight(Arc<LoopState>);

impl Drop for InFlight {
    fn drop(&mut self) {
        self.0.in_flight.fetch_sub(1, SeqCst);
    }
}

/// Tells the queue when a loop's thread sleeps and wakes; the thread's event loop calls it
/// (`kj_rs_tokio::ParkHooks`).
pub struct Parking<T> {
    shared: Arc<Shared<T>>,
    me: Arc<LoopState>,
}

impl<T> Parking<T> {
    /// The thread has nothing to run and is about to sleep. If work is queued the loop is
    /// granted a take, and its taker is woken, which the thread then runs instead of sleeping.
    pub fn before_park(&self) {
        let me = &self.me;
        if me.gone.load(SeqCst) {
            return;
        }
        me.asleep.store(true, SeqCst);
        // A push that has not seen this loop asleep has queued its item by now.
        if self.shared.queued.load(SeqCst) > 0 && me.asleep.swap(false, SeqCst) {
            me.grant();
        }
    }

    /// The thread woke.
    pub fn after_unpark(&self) {
        self.me.asleep.store(false, SeqCst);
    }
}

#[cfg(test)]
#[path = "queue-test.rs"]
mod tests;
