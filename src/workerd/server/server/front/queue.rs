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
//! suspended from collecting more while others have none; among equals it is the loop that
//! went to sleep last, so a server with little to do keeps serving on a thread that is warm
//! and leaves the others asleep.
//! It wakes at most that one loop per
//! item, and none if every loop is awake: each of those looks at the queue before it sleeps.
//! Either the push sees the loop asleep or the loop sees the item queued; both read after they
//! wrote (`SeqCst`), so not both miss.
//!
//! A loop that is woken for an item is granted one take, which the task that takes for it uses
//! when it next runs. That is not always at once. The thread may have woken at the same moment
//! for something of its own, a timer or an event of another thread, and its event loop then
//! runs that first ([`Parking::before_resume`]), for as long as it takes; so a loop that goes
//! on to other work with its grant unused wakes another sleeping loop for the item, and takes
//! it itself only if it is still there when its taker runs. And a loop's taker may not be
//! running at all, as when the future that holds it is not being polled: such a loop comes
//! back to sleep with its grant unused ([`Parking::before_park`]), hands the item on the same
//! way, and is not counted asleep again, so nothing wakes it for an item, until its taker runs
//! and uses the grant. An item therefore never waits on a loop while another sleeps.
//!
//! There is no cap on what a loop has in flight: a cap would leave work queued behind
//! long-lived items (WebSockets, long polls) with loops free to serve it.
//!
//! There is one on what is queued and not yet taken (the queue's limit). Loops that are all
//! busy take nothing, and whoever pushes must not go on collecting what clients send: a push
//! waits until the queue has room ([`Queue::push`], [`Queue::room`]), in the order the pushes
//! were made, and what it would have queued stays with its client meanwhile. An item counts
//! from its push until a loop takes it or its ticket takes it out; what a loop has taken does
//! not count, so the limit never keeps a loop from taking.
//!
//! An item leaves the queue in one of three ways. A loop takes it ([`Worker::poll_take`]), which
//! is one step under the queue's lock: a loop that has stopped taking (a dropped [`Worker`])
//! never holds an item it will not serve, and what it leaves queued is for the others, one of
//! which it wakes. Its [`Ticket`] is dropped while it is queued, which is how the request of a
//! client that went away leaves. Or the last loop is gone, and the queue drops what it holds and
//! is given.
//!
//! Every one of them is a step whose cost does not depend on how much is queued ([`Items`]),
//! so many clients that leave a long queue at once hold neither the front nor the loops up.

use std::cmp::Reverse;
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
use tokio::sync::Semaphore;

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
    /// The queue's room: a permit for each item that may still be queued. A push takes one
    /// for its item, and whatever takes the item out of the queue gives it back. Closed once
    /// the queue takes no more items, so that nobody waits for room.
    room: Semaphore,
    /// How many items are queued, for a loop about to sleep to read without the lock.
    queued: AtomicUsize,
    /// Every item with a smaller id has been taken: a ticket for one has nothing to remove.
    taken_through: AtomicU64,
    /// The queue is closed and empty: the loops are done with it.
    ended: AtomicBool,
    /// Counts the times a loop went to sleep, to tell which did last.
    sleeps: AtomicU64,
}

/// The queued items, in the order they were pushed.
///
/// Each is in a slot of a table that only grows, linked to the items before and after it, so
/// that an item leaves from anywhere in the queue in one step and its slot is used again. A
/// ticket names its item's slot, and its id tells the item from a later one in the same slot.
struct Items<T> {
    slots: Vec<Slot<T>>,
    /// The slots that hold no item.
    free: Vec<usize>,
    /// The oldest and the newest item, if there is any.
    ends: Option<(usize, usize)>,
    len: usize,
}

struct Slot<T> {
    /// The item and its id; `None` for a free slot.
    item: Option<(u64, T)>,
    /// The slots of the items before and after this one.
    before: Option<usize>,
    after: Option<usize>,
}

impl<T> Items<T> {
    const fn new() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
            ends: None,
            len: 0,
        }
    }

    const fn len(&self) -> usize {
        self.len
    }

    const fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Adds `item` behind the others, and returns its slot.
    fn push_back(&mut self, id: u64, item: T) -> usize {
        let newest = self.ends.map(|(_, newest)| newest);
        let slot = Slot {
            item: Some((id, item)),
            before: newest,
            after: None,
        };
        let index = if let Some(index) = self.free.pop() {
            self.slots[index] = slot;
            index
        } else {
            self.slots.push(slot);
            self.slots.len() - 1
        };
        match self.ends {
            Some((oldest, newest)) => {
                self.slots[newest].after = Some(index);
                self.ends = Some((oldest, index));
            }
            None => self.ends = Some((index, index)),
        }
        self.len += 1;
        index
    }

    /// Takes the oldest item out.
    fn pop_front(&mut self) -> Option<(u64, T)> {
        let (oldest, _) = self.ends?;
        self.unlink(oldest)
    }

    /// Takes the item `id` out of `slot`, if it is still there.
    fn remove(&mut self, slot: usize, id: u64) -> Option<T> {
        match self.slots.get(slot)?.item {
            Some((held, _)) if held == id => self.unlink(slot).map(|(_, item)| item),
            _ => None,
        }
    }

    /// Takes the item of `index` out of the order and frees its slot.
    fn unlink(&mut self, index: usize) -> Option<(u64, T)> {
        let slot = &mut self.slots[index];
        let item = slot.item.take()?;
        let (before, after) = (slot.before.take(), slot.after.take());
        if let Some(before) = before {
            self.slots[before].after = after;
        }
        if let Some(after) = after {
            self.slots[after].before = before;
        }
        self.ends = match (self.ends, before, after) {
            (Some((oldest, newest)), _, _) if oldest == index && newest == index => None,
            (Some((_, newest)), None, Some(after)) => Some((after, newest)),
            (Some((oldest, _)), Some(before), None) => Some((oldest, before)),
            (ends, _, _) => ends,
        };
        self.free.push(index);
        self.len -= 1;
        Some(item)
    }

    /// Takes every item out, oldest first.
    fn take_all(&mut self) -> Vec<T> {
        let mut items = Vec::with_capacity(self.len);
        while let Some((_, item)) = self.pop_front() {
            items.push(item);
        }
        items
    }
}

struct State<T> {
    /// The items, whose ids ascend.
    items: Items<T>,
    next_id: u64,
    /// No more items will be pushed.
    closed: bool,
    /// There were loops, and the last is gone: nothing will be taken.
    deserted: bool,
    loops: Vec<Arc<LoopState>>,
}

/// What the queue knows of one loop.
#[derive(Default)]
struct LoopState {
    /// The loop's thread sleeps, or is about to, and has not been woken for an item. Never set
    /// for a loop whose taker does not run.
    asleep: AtomicBool,
    /// When the loop last went to sleep, as a count of all loops' sleeps.
    slept: AtomicU64,
    /// The loop may take one item. Set until its taker runs.
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
    /// asleep: the caller wakes it. Among loops with equally few it is the one that went to
    /// sleep last, whose thread and isolates are the likeliest to still be warm.
    fn claim_sleeper(&self) -> Option<Arc<LoopState>> {
        loop {
            let sleeper = self
                .loops
                .iter()
                .filter(|worker| worker.asleep.load(SeqCst))
                .min_by_key(|worker| {
                    let in_flight = worker.in_flight.load(SeqCst);
                    (in_flight, Reverse(worker.slept.load(SeqCst)))
                })?;
            // The loop may have woken by itself meanwhile; then another is chosen.
            if sleeper.asleep.swap(false, SeqCst) {
                return Some(Arc::clone(sleeper));
            }
        }
    }
}

impl<T> Queue<T> {
    /// A queue that holds `limit` items at most (one at least).
    #[must_use]
    pub fn new(limit: usize) -> Self {
        let limit = limit.clamp(1, Semaphore::MAX_PERMITS);
        Self {
            shared: Arc::new(Shared {
                room: Semaphore::new(limit),
                state: Mutex::new(State {
                    items: Items::new(),
                    next_id: 0,
                    closed: false,
                    deserted: false,
                    loops: Vec::new(),
                }),
                queued: AtomicUsize::new(0),
                taken_through: AtomicU64::new(0),
                ended: AtomicBool::new(false),
                sleeps: AtomicU64::new(0),
            }),
        }
    }

    /// Queues `item` behind what is queued, once the queue has room for it, and wakes a
    /// sleeping loop for it if there is one. Pushes that wait for room get it in the order
    /// they were made. Dropping the future drops the item, queued or not.
    ///
    /// The item stays queued for as long as the ticket is kept, or until a loop takes it. A
    /// queue that is closed or that the last loop has left drops the item instead, without
    /// waiting.
    pub async fn push(&self, item: T) -> Ticket<T> {
        let shared = &self.shared;
        // The item has its room from here, and whoever takes it out of the queue frees it. A
        // closed semaphore is a queue that takes no more.
        let admitted = shared
            .room
            .acquire()
            .await
            .map(tokio::sync::SemaphorePermit::forget)
            .is_ok();
        let mut state = shared.lock();
        let id = state.next_id;
        state.next_id += 1;
        if !admitted || state.closed || state.deserted {
            drop(state);
            if admitted {
                shared.room.add_permits(1);
            }
            drop(item);
            return Ticket {
                shared: Arc::clone(shared),
                id,
                slot: None,
            };
        }
        let slot = state.items.push_back(id, item);
        shared.changed(&state);
        let sleeper = state.claim_sleeper();
        drop(state);
        if let Some(sleeper) = sleeper {
            sleeper.grant();
        }
        Ticket {
            shared: Arc::clone(shared),
            id,
            slot: Some(slot),
        }
    }

    /// Resolves once the queue has room for an item, or takes no more. The room is not kept:
    /// this is for a caller that would bring more work in, such as an accept loop, to wait
    /// behind the pushes that already wait.
    pub async fn room(&self) {
        // Closed is as good as room: nothing waits for a queue that takes no more.
        drop(self.shared.room.acquire().await);
    }

    /// Says that nothing more will be pushed: a push made or waiting from here on drops its
    /// item. The loops take what is queued, and are then done ([`Worker::poll_take`]).
    pub fn close(&self) {
        let mut state = self.shared.lock();
        state.closed = true;
        self.shared.changed(&state);
        drop(state);
        self.shared.room.close();
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

/// Keeps an item queued: dropping it removes the item if no loop has taken it.
pub struct Ticket<T> {
    shared: Arc<Shared<T>>,
    id: u64,
    /// The item's slot in the queue; `None` once the ticket has nothing to remove.
    slot: Option<usize>,
}

impl<T> Ticket<T> {
    /// Leaves the item queued until a loop takes it, whatever becomes of who queued it.
    pub fn keep_queued(mut self) {
        self.slot = None;
    }
}

impl<T> Drop for Ticket<T> {
    fn drop(&mut self) {
        let Some(slot) = self.slot else {
            return;
        };
        if self.id < self.shared.taken_through.load(SeqCst) {
            return;
        }
        let mut state = self.shared.lock();
        let Some(removed) = state.items.remove(slot, self.id) else {
            return;
        };
        self.shared.changed(&state);
        drop(state);
        self.shared.room.add_permits(1);
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
        self.me.taker.register(cx.waker());
        if self.shared.ended.load(SeqCst) {
            return Poll::Ready(None);
        }
        if !self.me.granted.swap(false, SeqCst) {
            return Poll::Pending;
        }
        let mut state = self.shared.lock();
        let Some((id, item)) = state.items.pop_front() else {
            // Another loop was quicker, or the client went away.
            return Poll::Pending;
        };
        self.shared.taken_through.store(id + 1, SeqCst);
        self.me.in_flight.fetch_add(1, SeqCst);
        self.shared.changed(&state);
        drop(state);
        // Which may wake a push that waits for room.
        self.shared.room.add_permits(1);
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
        self.me.gone.store(true, SeqCst);
        self.me.asleep.store(false, SeqCst);
        let mut state = self.shared.lock();
        state.loops.retain(|worker| !Arc::ptr_eq(worker, &self.me));
        if state.loops.is_empty() {
            state.deserted = true;
            let abandoned = state.items.take_all();
            self.shared.changed(&state);
            drop(state);
            self.shared.room.close();
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

/// Tells the queue when a loop's thread sleeps, wakes, and goes on to other work; the thread's
/// event loop calls it (`kj_rs_tokio::ParkHooks`).
pub struct Parking<T> {
    shared: Arc<Shared<T>>,
    me: Arc<LoopState>,
}

impl<T> Parking<T> {
    /// The thread has nothing to run and is about to sleep. If work is queued the loop is
    /// granted a take, and its taker is woken, which the thread then runs instead of sleeping.
    ///
    /// A loop that still has a grant has a taker that does not run: the thread ran everything
    /// that was ready before it got here, and the taker was woken with the grant. Such a loop
    /// sleeps without being counted asleep, and what it was granted is another loop's to take.
    pub fn before_park(&self) {
        let me = &self.me;
        if me.gone.load(SeqCst) {
            return;
        }
        if me.granted.load(SeqCst) {
            self.hand_on();
            return;
        }
        me.slept
            .store(self.shared.sleeps.fetch_add(1, SeqCst), SeqCst);
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

    /// The thread goes on to other work, which may keep it for long, and may do so before
    /// its taker has run: a loop that still has its grant wakes a sleeping loop for the item.
    /// Its own taker uses the grant when it runs, if the item is still queued then.
    pub fn before_resume(&self) {
        if self.me.granted.load(SeqCst) {
            self.hand_on();
        }
    }

    /// Wakes a sleeping loop, if there is one, for an item this loop was granted and has not
    /// taken.
    fn hand_on(&self) {
        if self.shared.queued.load(SeqCst) == 0 {
            return;
        }
        let state = self.shared.lock();
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

#[cfg(test)]
#[path = "queue-test.rs"]
mod tests;
