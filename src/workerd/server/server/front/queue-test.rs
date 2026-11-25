// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::atomic::AtomicUsize;
use std::task::Wake;
use std::task::Waker;
use std::thread;
use std::time::Duration;

use super::*;

/// A taker's waker that counts its wake-ups and unparks a thread.
struct Wakes {
    count: AtomicUsize,
    /// The wake-ups that came from a thread other than `thread`.
    foreign: AtomicUsize,
    thread: Option<thread::Thread>,
}

impl Wakes {
    fn new(thread: Option<thread::Thread>) -> Arc<Self> {
        Arc::new(Self {
            count: AtomicUsize::new(0),
            foreign: AtomicUsize::new(0),
            thread,
        })
    }
}

impl Wake for Wakes {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.count.fetch_add(1, SeqCst);
        if let Some(thread) = &self.thread {
            if thread.id() != thread::current().id() {
                self.foreign.fetch_add(1, SeqCst);
            }
            thread.unpark();
        }
    }
}

/// Plays a loop thread on the calling thread until the queue ends: takes what it may, and
/// sleeps whenever it took nothing, telling the queue as an event loop would. Returns what it
/// took and how often another thread woke it.
fn run_loop(worker: &Worker<u32>) -> (Vec<u32>, usize) {
    let parking = worker.parking();
    let counted = Wakes::new(Some(thread::current()));
    let waker = Waker::from(Arc::clone(&counted));
    let mut cx = Context::from_waker(&waker);
    let mut taken = Vec::new();
    loop {
        match worker.poll_take(&mut cx) {
            Poll::Ready(Some((item, in_flight))) => {
                taken.push(item);
                drop(in_flight);
                continue;
            }
            Poll::Ready(None) => break,
            Poll::Pending => {}
        }
        parking.before_park();
        // A wake-up since the poll left a token, with which this returns at once.
        thread::park_timeout(Duration::from_mins(1));
        parking.after_unpark();
    }
    (taken, counted.foreign.load(SeqCst))
}

/// A loop for a test that plays its thread itself: its worker, its parking, and its wake-ups.
struct TestLoop {
    worker: Worker<u32>,
    parking: Parking<u32>,
    wakes: Arc<Wakes>,
}

impl TestLoop {
    fn new(queue: &Queue<u32>) -> Self {
        let worker = queue.worker();
        let parking = worker.parking();
        let wakes = Wakes::new(None);
        let this = Self {
            worker,
            parking,
            wakes,
        };
        // The loop's taker is polled once as the loop starts, which is when it leaves its waker.
        assert!(this.takes_nothing());
        this
    }

    fn poll(&self) -> Poll<Option<(u32, InFlight)>> {
        let waker = Waker::from(Arc::clone(&self.wakes));
        self.worker.poll_take(&mut Context::from_waker(&waker))
    }

    /// The item the loop takes when polled; panics if it takes none.
    fn take(&self) -> (u32, InFlight) {
        match self.poll() {
            Poll::Ready(Some(taken)) => taken,
            Poll::Ready(None) => panic!("the queue ended"),
            Poll::Pending => panic!("nothing was taken"),
        }
    }

    fn takes_nothing(&self) -> bool {
        self.poll().is_pending()
    }

    fn wakes(&self) -> usize {
        self.wakes.count.load(SeqCst)
    }

    /// The loop runs out of things to do and sleeps, as its event loop would have it.
    fn sleep(&self) {
        assert!(self.takes_nothing());
        self.parking.before_park();
    }
}

#[test]
fn a_loop_takes_only_when_it_has_nothing_else_to_do() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    let _first = queue.push(1);
    let _second = queue.push(2);
    assert_eq!(queue.len(), 2);
    // The loop is awake: nobody wakes it, and a poll for another reason takes nothing.
    assert_eq!(worker.wakes(), 0);
    assert!(worker.takes_nothing());
    // About to sleep, it is granted one take, first in first out, and one only.
    worker.parking.before_park();
    assert_eq!(worker.wakes(), 1);
    worker.parking.after_unpark();
    assert_eq!(worker.take().0, 1);
    assert!(worker.takes_nothing());
    worker.parking.before_park();
    assert_eq!(worker.take().0, 2);
    assert!(queue.is_empty());
    // With nothing queued it sleeps.
    worker.parking.before_park();
    assert_eq!(worker.wakes(), 2);
    assert!(worker.takes_nothing());
}

#[test]
fn a_push_wakes_one_sleeping_loop_and_no_loop_that_is_awake() {
    let queue = Queue::new();
    let loops: Vec<_> = (0..4).map(|_| TestLoop::new(&queue)).collect();
    let total = |loops: &[TestLoop]| loops.iter().map(TestLoop::wakes).sum::<usize>();
    // Every loop is awake.
    let _first = queue.push(1);
    assert_eq!(total(&loops), 0);
    // Three sleep; the first to find the item queued takes it instead.
    loops[1].parking.before_park();
    assert_eq!(loops[1].wakes(), 1);
    assert_eq!(loops[1].take().0, 1);
    for worker in &loops[1..] {
        worker.sleep();
    }
    assert_eq!(total(&loops), 1);
    // Each push wakes one of them, and a loop that was woken is not woken again.
    let _second = queue.push(2);
    assert_eq!(total(&loops), 2);
    let _third = queue.push(3);
    let _fourth = queue.push(4);
    assert_eq!(total(&loops), 4);
    assert!(loops[1..].iter().all(|worker| worker.wakes() <= 2));
    // No loop sleeps any more: a further push wakes none.
    let _fifth = queue.push(5);
    assert_eq!(total(&loops), 4);
    let mut taken: Vec<u32> = loops[1..].iter().map(|worker| worker.take().0).collect();
    taken.sort_unstable();
    assert_eq!(taken, [2, 3, 4]);
    assert_eq!(queue.len(), 1);
}

#[test]
fn a_push_prefers_the_sleeping_loop_with_the_fewest_items_in_flight() {
    let queue = Queue::new();
    let busy = TestLoop::new(&queue);
    let free = TestLoop::new(&queue);
    // `busy` holds two items whose requests are suspended.
    let _a = queue.push(1);
    let _b = queue.push(2);
    busy.parking.before_park();
    let first = busy.take();
    busy.parking.before_park();
    let second = busy.take();
    busy.sleep();
    free.sleep();
    let (busy_wakes, free_wakes) = (busy.wakes(), free.wakes());
    let _c = queue.push(3);
    assert_eq!((busy.wakes(), free.wakes()), (busy_wakes, free_wakes + 1));
    assert_eq!(free.take().0, 3);
    // Once its items are done, `busy` is the one with fewer in flight: `free` still holds one.
    drop((first, second));
    let held = free.take_after_push(&queue, 4);
    busy.sleep();
    free.sleep();
    let (busy_wakes, free_wakes) = (busy.wakes(), free.wakes());
    let _e = queue.push(5);
    assert_eq!((busy.wakes(), free.wakes()), (busy_wakes + 1, free_wakes));
    drop(held);
}

impl TestLoop {
    /// Pushes `item` and has this loop, about to sleep, take it.
    fn take_after_push(&self, queue: &Queue<u32>, item: u32) -> InFlight {
        // The item stays queued without its ticket only until it is taken, which is now.
        let ticket = queue.push(item);
        self.parking.before_park();
        let (taken, in_flight) = self.take();
        assert_eq!(taken, item);
        drop(ticket);
        in_flight
    }
}

#[test]
fn of_equally_loaded_sleeping_loops_the_last_to_sleep_is_woken() {
    let queue = Queue::new();
    let loops: Vec<_> = (0..3).map(|_| TestLoop::new(&queue)).collect();
    for worker in &loops {
        worker.parking.before_park();
    }
    // The loop that serves an item sleeps last again, so one item at a time stays on it.
    for item in 0..4 {
        let _ticket = queue.push(item);
        assert_eq!(loops[2].wakes(), item as usize + 1);
        assert_eq!(loops[2].take().0, item);
        loops[2].sleep();
    }
    assert_eq!((loops[0].wakes(), loops[1].wakes()), (0, 0));
    // An item that arrives while that loop is busy goes to the one that slept last before it.
    let _ticket = queue.push(10);
    let held = loops[2].take();
    let _ticket = queue.push(11);
    assert_eq!((loops[0].wakes(), loops[1].wakes()), (0, 1));
    assert_eq!(loops[1].take().0, 11);
    drop(held);
}

#[test]
fn a_dropped_ticket_takes_its_item_out_of_the_queue() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    let first = queue.push(1);
    let second = queue.push(2);
    let third = queue.push(3);
    drop(second);
    assert_eq!(queue.len(), 2);
    worker.parking.before_park();
    assert_eq!(worker.take().0, 1);
    // The ticket of an item that was taken removes nothing.
    drop(first);
    assert_eq!(queue.len(), 1);
    drop(third);
    assert!(queue.is_empty());
    // The loop was not told of an item that is gone.
    worker.parking.before_park();
    assert!(worker.takes_nothing());
}

#[test]
fn a_loop_woken_for_an_item_that_left_takes_nothing() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    worker.sleep();
    let ticket = queue.push(1);
    assert_eq!(worker.wakes(), 1);
    drop(ticket);
    assert!(worker.takes_nothing());
}

#[test]
fn a_loop_that_stops_taking_leaves_the_queue_to_the_others() {
    let queue = Queue::new();
    let leaving = TestLoop::new(&queue);
    let staying = TestLoop::new(&queue);
    leaving.sleep();
    staying.sleep();
    // The push wakes one of the two; whichever it was, the item is the other's once the first
    // is gone.
    let _ticket = queue.push(1);
    let woken_first = if leaving.wakes() == 1 {
        leaving
    } else {
        staying
    };
    let other = queue.worker();
    drop(woken_first);
    assert_eq!(queue.len(), 1);
    let waker = Waker::noop();
    // `other` never slept, so it finds the item when it next would.
    other.parking().before_park();
    assert!(matches!(
        other.poll_take(&mut Context::from_waker(waker)),
        Poll::Ready(Some((1, _)))
    ));
}

#[test]
fn a_leaving_loop_wakes_a_sleeper_for_what_it_leaves_queued() {
    let queue = Queue::new();
    let sleeper = TestLoop::new(&queue);
    let leaving = TestLoop::new(&queue);
    // `sleeper` has an item in flight, so of the two sleeping loops the push wakes `leaving`.
    let _in_flight = sleeper.take_after_push(&queue, 0);
    sleeper.sleep();
    leaving.sleep();
    let wakes = sleeper.wakes();
    let _ticket = queue.push(1);
    assert_eq!((leaving.wakes(), sleeper.wakes()), (1, wakes));
    // It leaves instead of taking the item, which nobody else was woken for.
    drop(leaving);
    assert_eq!(sleeper.wakes(), wakes + 1);
    assert_eq!(sleeper.take().0, 1);
}

#[test]
fn a_closed_queue_is_served_to_its_end() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    let idle = TestLoop::new(&queue);
    let _first = queue.push(1);
    idle.sleep();
    queue.close();
    // What was queued is still taken; an item pushed after the close is dropped.
    let _late = queue.push(2);
    assert_eq!(queue.len(), 1);
    worker.parking.before_park();
    assert_eq!(worker.take().0, 1);
    // The queue is closed and empty: every loop is told, and done.
    assert!(idle.wakes() >= 1);
    assert!(matches!(idle.poll(), Poll::Ready(None)));
    assert!(matches!(worker.poll(), Poll::Ready(None)));
}

#[test]
fn closing_an_empty_queue_ends_the_loops_at_once() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    worker.sleep();
    queue.close();
    assert_eq!(worker.wakes(), 1);
    assert!(matches!(worker.poll(), Poll::Ready(None)));
}

/// Counts its drops.
struct Dropped(Arc<AtomicUsize>);

impl Drop for Dropped {
    fn drop(&mut self) {
        self.0.fetch_add(1, SeqCst);
    }
}

#[test]
fn a_queue_without_loops_drops_its_items() {
    let drops = Arc::new(AtomicUsize::new(0));
    let queue = Queue::new();
    let worker = queue.worker();
    let _queued = queue.push(Dropped(Arc::clone(&drops)));
    assert_eq!(drops.load(SeqCst), 0);
    drop(worker);
    assert_eq!(drops.load(SeqCst), 1);
    let _refused = queue.push(Dropped(Arc::clone(&drops)));
    assert_eq!(drops.load(SeqCst), 2);
    assert!(queue.is_empty());
}

#[test]
fn items_leave_from_anywhere_and_the_rest_keep_their_order() {
    let queue = Queue::new();
    let worker = TestLoop::new(&queue);
    let mut tickets: Vec<_> = (0..10).map(|item| Some(queue.push(item))).collect();
    // The newest, the oldest, and some in between, next to each other and apart.
    for item in [9, 0, 4, 5, 7] {
        tickets[item] = None;
    }
    assert_eq!(queue.len(), 5);
    // What is pushed now takes the places of those that left, behind every older item.
    let _late: Vec<_> = (10..13).map(|item| queue.push(item)).collect();
    let mut taken = Vec::new();
    for _ in 0..8 {
        worker.parking.before_park();
        taken.push(worker.take().0);
    }
    assert_eq!(taken, [1, 2, 3, 6, 8, 10, 11, 12]);
    assert!(queue.is_empty());
    // A ticket whose item was taken, and whose place another item may have, removes nothing.
    let _next = queue.push(13);
    drop(tickets);
    assert_eq!(queue.len(), 1);
    worker.parking.before_park();
    assert_eq!(worker.take().0, 13);
}

/// Many requests that leave a long queue at once: the cost of one leaving does not grow with
/// the queue. Were it to (an item taken out of the middle of an array moves the items behind
/// it), this many would take the better part of a minute.
#[test]
fn a_long_queue_is_left_quickly() {
    const ITEMS: usize = 200_000;
    // About as large as the work the front queues.
    type Item = [usize; 32];
    let queue = Queue::<Item>::new();
    let _worker = queue.worker();
    let mut tickets: Vec<_> = (0..ITEMS)
        .map(|item| Some(queue.push([item; 32])))
        .collect();
    let start = std::time::Instant::now();
    // From the middle outwards, which is as far from both ends as an item can be.
    for step in 0..ITEMS / 2 {
        tickets[ITEMS / 2 + step] = None;
        tickets[ITEMS / 2 - 1 - step] = None;
    }
    assert!(queue.is_empty());
    let elapsed = start.elapsed();
    assert!(elapsed < Duration::from_secs(10), "{elapsed:?}");
}

/// Loop threads that sleep whenever they have taken nothing, and pushers that push as fast as
/// they can: every item is taken exactly once, which a lost wake-up would prevent (the test
/// would hang), and a push wakes one loop at most.
#[test]
fn nothing_is_lost_and_no_loop_is_woken_in_vain_under_contention() {
    const LOOPS: usize = 6;
    const PUSHERS: usize = 4;
    const ITEMS: u32 = 20_000;
    let queue = Queue::<u32>::new();
    let taken = Arc::new(Mutex::new(Vec::new()));
    let woken = Arc::new(AtomicUsize::new(0));
    // Every loop is registered before the first push: an item is never dropped as deserted.
    let workers: Vec<_> = (0..LOOPS).map(|_| queue.worker()).collect();
    let mut loops = Vec::new();
    for worker in workers {
        let taken = Arc::clone(&taken);
        let woken = Arc::clone(&woken);
        loops.push(thread::spawn(move || {
            let (mine, foreign) = run_loop(&worker);
            woken.fetch_add(foreign, SeqCst);
            taken.lock().unwrap().extend(mine);
        }));
    }
    let mut pushing = Vec::new();
    for pusher in 0..PUSHERS as u32 {
        let queue = queue.clone();
        // The tickets are kept until every item was pushed, as a connection keeps one for as
        // long as its request is in flight.
        pushing.push(thread::spawn(move || {
            (0..ITEMS)
                .map(|i| queue.push(pusher * ITEMS + i))
                .collect::<Vec<_>>()
        }));
    }
    let mut tickets = Vec::new();
    for pusher in pushing {
        tickets.push(pusher.join().unwrap());
    }
    queue.close();
    for worker in loops {
        worker.join().unwrap();
    }
    drop(tickets);
    let mut taken = Arc::try_unwrap(taken).unwrap().into_inner().unwrap();
    taken.sort_unstable();
    let expected: Vec<u32> = (0..PUSHERS as u32 * ITEMS).collect();
    assert_eq!(taken, expected);
    // Another thread woke a loop once per item at most, and each loop once for the close.
    let items = PUSHERS * ITEMS as usize;
    assert!(woken.load(SeqCst) <= items + LOOPS, "{woken:?}");
}

#[test]
fn one_pusher_sees_its_items_taken_in_order() {
    let queue = Queue::<u32>::new();
    let worker = queue.worker();
    let pusher = {
        thread::spawn(move || {
            let tickets: Vec<_> = (0..5_000).map(|i| queue.push(i)).collect();
            queue.close();
            tickets
        })
    };
    let (taken, _) = run_loop(&worker);
    drop(pusher.join().unwrap());
    assert_eq!(taken, (0..5_000).collect::<Vec<u32>>());
}
