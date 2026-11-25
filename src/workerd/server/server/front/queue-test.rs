// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::Barrier;
use std::sync::atomic::AtomicUsize;
use std::task::Wake;
use std::task::Waker;
use std::thread;
use std::time::Duration;
use std::time::Instant;

use futures::FutureExt;

use super::*;

/// More than any test queues.
const ROOMY: usize = 1 << 20;

impl<T> Queue<T> {
    /// Pushes `item` onto a queue that has room for it.
    fn push_now(&self, item: T) -> Ticket<T> {
        self.push(item).now_or_never().expect("the queue has room")
    }
}

/// A taker's waker that counts its wake-ups.
struct Wakes {
    count: AtomicUsize,
}

impl Wakes {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            count: AtomicUsize::new(0),
        })
    }
}

impl Wake for Wakes {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.count.fetch_add(1, SeqCst);
    }
}

/// Unparks a loop's thread.
struct Unparker(thread::Thread);

impl Wake for Unparker {
    fn wake(self: Arc<Self>) {
        self.0.unpark();
    }
}

/// Plays a loop thread on the calling thread until the queue ends: takes what it may, and
/// sleeps whenever it took nothing, telling the queue as an event loop would. Returns what it
/// took. It sleeps without a timeout: were a wake-up lost, it would sleep for good.
fn run_loop(worker: &Worker<u32>) -> Vec<u32> {
    let parking = worker.parking();
    let waker = Waker::from(Arc::new(Unparker(thread::current())));
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
        thread::park();
        parking.after_unpark();
    }
    taken
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
        let wakes = Wakes::new();
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
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    let _first = queue.push_now(1);
    let _second = queue.push_now(2);
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
    let queue = Queue::new(ROOMY);
    let loops: Vec<_> = (0..4).map(|_| TestLoop::new(&queue)).collect();
    let total = |loops: &[TestLoop]| loops.iter().map(TestLoop::wakes).sum::<usize>();
    // Every loop is awake.
    let _first = queue.push_now(1);
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
    let _second = queue.push_now(2);
    assert_eq!(total(&loops), 2);
    let _third = queue.push_now(3);
    let _fourth = queue.push_now(4);
    assert_eq!(total(&loops), 4);
    assert!(loops[1..].iter().all(|worker| worker.wakes() <= 2));
    // No loop sleeps any more: a further push wakes none.
    let _fifth = queue.push_now(5);
    assert_eq!(total(&loops), 4);
    let mut taken: Vec<u32> = loops[1..].iter().map(|worker| worker.take().0).collect();
    taken.sort_unstable();
    assert_eq!(taken, [2, 3, 4]);
    assert_eq!(queue.len(), 1);
}

#[test]
fn a_push_prefers_the_sleeping_loop_with_the_fewest_items_in_flight() {
    let queue = Queue::new(ROOMY);
    let busy = TestLoop::new(&queue);
    let free = TestLoop::new(&queue);
    // `busy` holds two items whose requests are suspended.
    let _a = queue.push_now(1);
    let _b = queue.push_now(2);
    busy.parking.before_park();
    let first = busy.take();
    busy.parking.before_park();
    let second = busy.take();
    busy.sleep();
    free.sleep();
    let (busy_wakes, free_wakes) = (busy.wakes(), free.wakes());
    let _c = queue.push_now(3);
    assert_eq!((busy.wakes(), free.wakes()), (busy_wakes, free_wakes + 1));
    assert_eq!(free.take().0, 3);
    // Once its items are done, `busy` is the one with fewer in flight: `free` still holds one.
    drop((first, second));
    let held = free.take_after_push(&queue, 4);
    busy.sleep();
    free.sleep();
    let (busy_wakes, free_wakes) = (busy.wakes(), free.wakes());
    let _e = queue.push_now(5);
    assert_eq!((busy.wakes(), free.wakes()), (busy_wakes + 1, free_wakes));
    drop(held);
}

impl TestLoop {
    /// Pushes `item` and has this loop, about to sleep, take it.
    fn take_after_push(&self, queue: &Queue<u32>, item: u32) -> InFlight {
        // The item stays queued without its ticket only until it is taken, which is now.
        let ticket = queue.push_now(item);
        self.parking.before_park();
        let (taken, in_flight) = self.take();
        assert_eq!(taken, item);
        drop(ticket);
        in_flight
    }
}

#[test]
fn of_equally_loaded_sleeping_loops_the_last_to_sleep_is_woken() {
    let queue = Queue::new(ROOMY);
    let loops: Vec<_> = (0..3).map(|_| TestLoop::new(&queue)).collect();
    for worker in &loops {
        worker.parking.before_park();
    }
    // The loop that serves an item sleeps last again, so one item at a time stays on it.
    for item in 0..4 {
        let _ticket = queue.push_now(item);
        assert_eq!(loops[2].wakes(), item as usize + 1);
        assert_eq!(loops[2].take().0, item);
        loops[2].sleep();
    }
    assert_eq!((loops[0].wakes(), loops[1].wakes()), (0, 0));
    // An item that arrives while that loop is busy goes to the one that slept last before it.
    let _ticket = queue.push_now(10);
    let held = loops[2].take();
    let _ticket = queue.push_now(11);
    assert_eq!((loops[0].wakes(), loops[1].wakes()), (0, 1));
    assert_eq!(loops[1].take().0, 11);
    drop(held);
}

#[test]
fn a_dropped_ticket_takes_its_item_out_of_the_queue() {
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    let first = queue.push_now(1);
    let second = queue.push_now(2);
    let third = queue.push_now(3);
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
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    worker.sleep();
    let ticket = queue.push_now(1);
    assert_eq!(worker.wakes(), 1);
    drop(ticket);
    assert!(worker.takes_nothing());
}

#[test]
fn a_loop_that_stops_taking_leaves_the_queue_to_the_others() {
    let queue = Queue::new(ROOMY);
    let leaving = TestLoop::new(&queue);
    let staying = TestLoop::new(&queue);
    leaving.sleep();
    staying.sleep();
    // The push wakes one of the two; whichever it was, the item is the other's once the first
    // is gone.
    let _ticket = queue.push_now(1);
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
    let queue = Queue::new(ROOMY);
    let sleeper = TestLoop::new(&queue);
    let leaving = TestLoop::new(&queue);
    // `sleeper` has an item in flight, so of the two sleeping loops the push wakes `leaving`.
    let _in_flight = sleeper.take_after_push(&queue, 0);
    sleeper.sleep();
    leaving.sleep();
    let wakes = sleeper.wakes();
    let _ticket = queue.push_now(1);
    assert_eq!((leaving.wakes(), sleeper.wakes()), (1, wakes));
    // It leaves instead of taking the item, which nobody else was woken for.
    drop(leaving);
    assert_eq!(sleeper.wakes(), wakes + 1);
    assert_eq!(sleeper.take().0, 1);
}

#[test]
fn a_closed_queue_is_served_to_its_end() {
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    let idle = TestLoop::new(&queue);
    let _first = queue.push_now(1);
    idle.sleep();
    queue.close();
    // What was queued is still taken; an item pushed after the close is dropped.
    let _late = queue.push_now(2);
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
    let queue = Queue::new(ROOMY);
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
    let queue = Queue::new(ROOMY);
    let worker = queue.worker();
    let _queued = queue.push_now(Dropped(Arc::clone(&drops)));
    assert_eq!(drops.load(SeqCst), 0);
    drop(worker);
    assert_eq!(drops.load(SeqCst), 1);
    let _refused = queue.push_now(Dropped(Arc::clone(&drops)));
    assert_eq!(drops.load(SeqCst), 2);
    assert!(queue.is_empty());
}

#[test]
fn a_loop_that_wakes_for_something_else_hands_its_grant_on() {
    let queue = Queue::new(ROOMY);
    let idle = TestLoop::new(&queue);
    let woken = TestLoop::new(&queue);
    idle.sleep();
    woken.sleep();
    // The push wakes the loop that slept last, whose thread wakes at that moment for another
    // reason and goes on to other work before its taker has run.
    let _ticket = queue.push_now(1);
    assert_eq!((woken.wakes(), idle.wakes()), (1, 0));
    woken.parking.after_unpark();
    woken.parking.before_resume();
    // The item is not left to wait for that work to end: the loop that still sleeps is woken.
    assert_eq!(idle.wakes(), 1);
    assert_eq!(idle.take().0, 1);
    // The first loop's taker runs when its thread next can, and finds nothing.
    assert!(woken.takes_nothing());
    // It takes part as before.
    idle.sleep();
    woken.sleep();
    let _ticket = queue.push_now(2);
    assert_eq!((woken.wakes(), idle.wakes()), (2, 1));
    assert_eq!(woken.take().0, 2);
}

#[test]
fn a_loop_keeps_a_grant_that_no_other_loop_can_use() {
    let queue = Queue::new(ROOMY);
    let busy = TestLoop::new(&queue);
    let woken = TestLoop::new(&queue);
    woken.sleep();
    let _ticket = queue.push_now(1);
    assert_eq!(woken.wakes(), 1);
    woken.parking.after_unpark();
    woken.parking.before_resume();
    // Nobody else sleeps: the loop takes the item itself once its taker runs.
    assert_eq!(busy.wakes(), 0);
    assert_eq!(woken.take().0, 1);
}

#[test]
fn a_loop_that_does_not_take_is_not_woken_for_more() {
    let queue = Queue::new(ROOMY);
    let taking = TestLoop::new(&queue);
    let stuck = TestLoop::new(&queue);
    taking.sleep();
    stuck.sleep();
    let _first = queue.push_now(1);
    assert_eq!((stuck.wakes(), taking.wakes()), (1, 0));
    // Its thread wakes, runs everything that was ready, and sleeps again without its taker
    // having run: what it was woken for goes to a loop that takes.
    stuck.parking.after_unpark();
    stuck.parking.before_park();
    assert_eq!((stuck.wakes(), taking.wakes()), (1, 1));
    assert_eq!(taking.take().0, 1);
    // While it does not take it is not woken again, by a push or by itself, though it is the
    // loop that went to sleep last.
    taking.sleep();
    let _second = queue.push_now(2);
    assert_eq!((stuck.wakes(), taking.wakes()), (1, 2));
    stuck.parking.after_unpark();
    stuck.parking.before_park();
    assert_eq!(stuck.wakes(), 1);
    // Once its taker runs again it takes what is queued, and is woken like any other loop.
    assert_eq!(stuck.take().0, 2);
    assert!(taking.takes_nothing());
    taking.sleep();
    stuck.sleep();
    let _third = queue.push_now(3);
    assert_eq!((stuck.wakes(), taking.wakes()), (2, 2));
    assert_eq!(stuck.take().0, 3);
}

/// A push that is still waiting for room, or the ticket of one that is not.
fn poll_push<'a>(queue: &'a Queue<u32>, item: u32) -> futures::future::BoxFuture<'a, Ticket<u32>> {
    let mut push = queue.push(item).boxed();
    assert!((&mut push).now_or_never().is_none(), "{item} was queued");
    push
}

#[test]
fn a_full_queue_holds_pushes_back_until_it_has_room() {
    let queue = Queue::new(2);
    let worker = TestLoop::new(&queue);
    let first = queue.push_now(1);
    let _second = queue.push_now(2);
    // The queue is full: further pushes wait, and so does whoever asks for room.
    let mut third = poll_push(&queue, 3);
    let mut fourth = poll_push(&queue, 4);
    let mut room = queue.room().boxed();
    assert!((&mut room).now_or_never().is_none());
    assert_eq!(queue.len(), 2);
    // A loop that takes an item makes room for the push that has waited longest, and for
    // that one only.
    worker.parking.before_park();
    assert_eq!(worker.take().0, 1);
    let _third = (&mut third).now_or_never().expect("the third is queued");
    assert!((&mut fourth).now_or_never().is_none());
    assert!((&mut room).now_or_never().is_none());
    assert_eq!(queue.len(), 2);
    // So does a ticket that takes its item out. The ticket of an item that was taken frees
    // nothing.
    drop(first);
    assert!((&mut fourth).now_or_never().is_none());
    drop(_third);
    let _fourth = (&mut fourth).now_or_never().expect("the fourth is queued");
    assert!((&mut room).now_or_never().is_none());
    // What the loops have taken does not count: with one item taken and not finished, the
    // queue has room once the next is taken.
    worker.parking.before_park();
    let (taken, _in_flight) = worker.take();
    assert_eq!(taken, 2);
    (&mut room).now_or_never().expect("the queue has room");
    // Asking for room took none of it.
    let _fifth = queue.push_now(5);
    assert_eq!(queue.len(), 2);
    worker.parking.before_park();
    assert_eq!(worker.take().0, 4);
    worker.parking.before_park();
    assert_eq!(worker.take().0, 5);
}

#[test]
fn a_push_that_is_given_up_while_it_waits_leaves_nothing_behind() {
    let drops = Arc::new(AtomicUsize::new(0));
    let queue = Queue::new(1);
    let worker = queue.worker();
    let _first = queue.push_now(Dropped(Arc::clone(&drops)));
    let mut second = queue.push(Dropped(Arc::clone(&drops))).boxed();
    assert!((&mut second).now_or_never().is_none());
    let mut third = queue.push(Dropped(Arc::clone(&drops))).boxed();
    assert!((&mut third).now_or_never().is_none());
    // Its client went away: the item is dropped, and its turn passes to the next.
    drop(second);
    assert_eq!(drops.load(SeqCst), 1);
    let waker = Waker::noop();
    worker.parking().before_park();
    assert!(matches!(
        worker.poll_take(&mut Context::from_waker(waker)),
        Poll::Ready(Some(_))
    ));
    let _third = (&mut third).now_or_never().expect("the third is queued");
    assert_eq!(queue.len(), 1);
}

#[test]
fn a_queue_that_takes_no_more_keeps_nobody_waiting() {
    let drops = Arc::new(AtomicUsize::new(0));
    // Closed, with an item still queued.
    let queue = Queue::new(1);
    let _worker = queue.worker();
    let _first = queue.push_now(Dropped(Arc::clone(&drops)));
    let mut waiting = queue.push(Dropped(Arc::clone(&drops))).boxed();
    assert!((&mut waiting).now_or_never().is_none());
    let mut room = queue.room().boxed();
    assert!((&mut room).now_or_never().is_none());
    queue.close();
    drop((&mut waiting).now_or_never().expect("the push is refused"));
    (&mut room).now_or_never().expect("nobody waits for room");
    assert_eq!((drops.load(SeqCst), queue.len()), (1, 1));

    // Left by its last loop.
    let queue = Queue::new(1);
    let worker = queue.worker();
    let _first = queue.push_now(Dropped(Arc::clone(&drops)));
    let mut waiting = queue.push(Dropped(Arc::clone(&drops))).boxed();
    assert!((&mut waiting).now_or_never().is_none());
    drop(worker);
    drop((&mut waiting).now_or_never().expect("the push is refused"));
    assert_eq!((drops.load(SeqCst), queue.len()), (3, 0));
}

#[test]
fn a_grant_for_an_item_that_left_is_handed_to_nobody() {
    let queue = Queue::new(ROOMY);
    let idle = TestLoop::new(&queue);
    let woken = TestLoop::new(&queue);
    idle.sleep();
    woken.sleep();
    let ticket = queue.push_now(1);
    assert_eq!((woken.wakes(), idle.wakes()), (1, 0));
    // The client went away before the loop that was woken for its request ran its taker.
    drop(ticket);
    woken.parking.after_unpark();
    woken.parking.before_resume();
    woken.parking.before_park();
    assert_eq!(idle.wakes(), 0);
    assert!(woken.takes_nothing());
}

#[test]
fn the_thread_of_a_loop_that_left_is_not_counted_asleep() {
    let queue = Queue::new(ROOMY);
    let staying = TestLoop::new(&queue);
    let leaving = TestLoop::new(&queue);
    let TestLoop {
        worker,
        parking,
        wakes,
    } = leaving;
    drop(worker);
    // Its thread goes on sleeping and waking for other things for as long as it runs.
    parking.before_park();
    staying.sleep();
    let _ticket = queue.push_now(1);
    assert_eq!((wakes.count.load(SeqCst), staying.wakes()), (0, 1));
    parking.after_unpark();
    parking.before_resume();
    assert_eq!(staying.take().0, 1);
}

#[test]
fn a_closed_queue_ends_when_its_last_item_is_taken_out_by_its_ticket() {
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    let ticket = queue.push_now(1);
    queue.close();
    // The loop is busy with other things, and the request's client goes away meanwhile.
    assert!(worker.takes_nothing());
    assert_eq!(worker.wakes(), 0);
    drop(ticket);
    // Nothing is left for the loops, which are told.
    assert_eq!(worker.wakes(), 1);
    assert!(matches!(worker.poll(), Poll::Ready(None)));
}

#[test]
fn a_queue_holds_one_item_at_least() {
    let queue = Queue::new(0);
    let _worker = queue.worker();
    let _first = queue.push_now(1);
    assert!(queue.push(2).now_or_never().is_none());
}

#[test]
fn items_leave_from_anywhere_and_the_rest_keep_their_order() {
    let queue = Queue::new(ROOMY);
    let worker = TestLoop::new(&queue);
    let mut tickets: Vec<_> = (0..10).map(|item| Some(queue.push_now(item))).collect();
    // The newest, the oldest, and some in between, next to each other and apart.
    for item in [9, 0, 4, 5, 7] {
        tickets[item] = None;
    }
    assert_eq!(queue.len(), 5);
    // What is pushed now takes the places of those that left, behind every older item.
    let _late: Vec<_> = (10..13).map(|item| queue.push_now(item)).collect();
    let mut taken = Vec::new();
    for _ in 0..8 {
        worker.parking.before_park();
        taken.push(worker.take().0);
    }
    assert_eq!(taken, [1, 2, 3, 6, 8, 10, 11, 12]);
    assert!(queue.is_empty());
    // A ticket whose item was taken, and whose place another item may have, removes nothing.
    let _next = queue.push_now(13);
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
    let queue = Queue::<Item>::new(ROOMY);
    let _worker = queue.worker();
    let mut tickets: Vec<_> = (0..ITEMS)
        .map(|item| Some(queue.push_now([item; 32])))
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

/// What became of each item of a stress run.
struct Fates {
    taken: Vec<AtomicUsize>,
    dropped: Vec<AtomicUsize>,
    /// How many items were dropped, whoever took them out of the queue.
    gone: AtomicUsize,
}

/// An item that records being dropped, taken or not.
struct Item {
    id: usize,
    fates: Arc<Fates>,
}

impl Drop for Item {
    fn drop(&mut self) {
        self.fates.dropped[self.id].fetch_add(1, SeqCst);
        self.fates.gone.fetch_add(1, SeqCst);
    }
}

/// A sequence of numbers that looks random and is the same for the same seed.
struct Dice(u64);

impl Dice {
    fn roll(&mut self, sides: u64) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0 % sides
    }
}

/// Plays a loop thread until the queue ends, or until it has taken `leaves_after` items, when
/// it drops its worker as a loop that stops does.
///
/// It sleeps without a timeout, so it sleeps for good if a wake-up it needs does not come.
/// Now and then its thread goes on to other work between waking and running its taker, and
/// now and then its taker does not run for a few sleeps of the thread, which wakes by itself
/// then, as a thread with other work does.
fn stress_loop(worker: Worker<Item>, mut dice: Dice, leaves_after: Option<usize>) {
    let parking = worker.parking();
    let waker = Waker::from(Arc::new(Unparker(thread::current())));
    let mut cx = Context::from_waker(&waker);
    let mut taken = 0;
    loop {
        match worker.poll_take(&mut cx) {
            Poll::Ready(Some((item, in_flight))) => {
                item.fates.taken[item.id].fetch_add(1, SeqCst);
                taken += 1;
                if leaves_after == Some(taken) {
                    return;
                }
                drop((item, in_flight));
                continue;
            }
            Poll::Ready(None) => return,
            Poll::Pending => {}
        }
        parking.before_park();
        // A wake-up since the poll left a token, with which this returns at once.
        thread::park();
        parking.after_unpark();
        match dice.roll(16) {
            0 => {
                parking.before_resume();
                thread::yield_now();
            }
            1 => {
                for _ in 0..3 {
                    parking.before_park();
                    thread::park_timeout(Duration::from_micros(50));
                    parking.after_unpark();
                }
            }
            _ => {}
        }
    }
}

/// One run of loops and pushers at once. Every item is either taken by exactly one loop or
/// taken out by its ticket, never both and never neither, though tickets are dropped while
/// loops take, loops leave while others are woken, and takers run late or not at all for a
/// while.
///
/// The pushers push in bursts, all at once, and between two bursts every item pushed so far
/// has to be gone from the queue: a wake-up that is lost leaves an item queued with every
/// loop asleep and nothing further pushed that would wake one, which fails the run.
///
/// Returns, of the items whose tickets were dropped, how many a loop took first and how many
/// the ticket took out.
fn stress(round: u64) -> (usize, usize) {
    const LEAVING: usize = 2;
    const PUSHERS: usize = 4;
    const BURSTS: usize = 150;
    const BURST: usize = 6;
    let most = PUSHERS * BURSTS * BURST;
    let counters = || (0..most).map(|_| AtomicUsize::new(0)).collect::<Vec<_>>();
    let fates = Arc::new(Fates {
        taken: counters(),
        dropped: counters(),
        gone: AtomicUsize::new(0),
    });
    let queue = Queue::<Item>::new(STRESS_LIMIT);
    // Few loops in some runs and several in others; two of them stop taking part of the way
    // through. Every loop is registered before the first push: an item is never dropped as
    // deserted.
    let loops = LEAVING + 1 + round as usize % 4;
    let workers: Vec<_> = (0..loops).map(|_| queue.worker()).collect();
    let (done, ended) = std::sync::mpsc::channel();
    for (index, worker) in workers.into_iter().enumerate() {
        let done = done.clone();
        let dice = Dice(round * 64 + index as u64 + 1);
        let leaves_after = (index < LEAVING).then_some(40 * (index + 1));
        thread::spawn(move || {
            stress_loop(worker, dice, leaves_after);
            done.send(()).unwrap();
        });
    }
    let pushed = Arc::new(AtomicUsize::new(0));
    let start = Arc::new(Barrier::new(PUSHERS + 1));
    let finish = Arc::new(Barrier::new(PUSHERS + 1));
    let mut pushing = Vec::new();
    for pusher in 0..PUSHERS {
        let queue = queue.clone();
        let fates = Arc::clone(&fates);
        let pushed = Arc::clone(&pushed);
        let (start, finish) = (Arc::clone(&start), Arc::clone(&finish));
        let mut dice = Dice(round * 64 + 32 + pusher as u64);
        pushing.push(thread::spawn(move || {
            let mut kept = Vec::new();
            for _ in 0..BURSTS {
                start.wait();
                // The tickets of requests that are in flight for a while.
                let mut held = Vec::new();
                for _ in 0..dice.roll(BURST as u64 + 1) {
                    let id = pushed.fetch_add(1, SeqCst);
                    // The queue is short, so a push often waits for a loop to make room.
                    let ticket = futures::executor::block_on(queue.push(Item {
                        id,
                        fates: Arc::clone(&fates),
                    }));
                    match dice.roll(4) {
                        // A client that goes away at once, or a little later, while loops
                        // take.
                        0 => drop(ticket),
                        1 => held.push(ticket),
                        // A client that stays until its request is served.
                        _ => kept.push((id, ticket)),
                    }
                    // The next push comes as the loops go back to sleep, which they do when
                    // they find the queue empty: a push at that moment is the one a loop
                    // could miss.
                    for _ in 0..10_000 {
                        if queue.is_empty() {
                            break;
                        }
                        std::hint::spin_loop();
                    }
                    for _ in 0..dice.roll(200) {
                        std::hint::spin_loop();
                    }
                }
                drop(held);
                finish.wait();
            }
            kept
        }));
    }
    let lost = |what: &str| -> ! {
        let gone = fates.gone.load(SeqCst);
        let pushed = pushed.load(SeqCst);
        panic!(
            "round {round}: {what} with {} items queued, {gone} of {pushed} gone: a wake-up \
             was lost",
            queue.len()
        )
    };
    for _ in 0..BURSTS {
        start.wait();
        finish.wait();
        let deadline = Instant::now() + Duration::from_secs(10);
        while fates.gone.load(SeqCst) < pushed.load(SeqCst) {
            if Instant::now() > deadline {
                lost("the loops sleep");
            }
            thread::yield_now();
        }
    }
    let mut kept = Vec::new();
    for pusher in pushing {
        kept.extend(pusher.join().unwrap());
    }
    queue.close();
    for _ in 0..loops {
        if ended.recv_timeout(Duration::from_secs(10)).is_err() {
            lost("a loop did not end");
        }
    }
    let total = pushed.load(SeqCst);
    let count = |counts: &[AtomicUsize], id: usize| counts[id].load(SeqCst);
    for id in 0..total {
        assert_eq!(count(&fates.dropped, id), 1, "item {id} of round {round}");
        assert!(count(&fates.taken, id) <= 1, "item {id} of round {round}");
    }
    for (id, _) in &kept {
        assert_eq!(count(&fates.taken, *id), 1, "item {id} of round {round}");
    }
    let taken: usize = (0..total).map(|id| count(&fates.taken, id)).sum();
    (taken - kept.len(), total - taken)
}

#[test]
fn nothing_is_lost_or_taken_twice_under_contention() {
    let (mut taken, mut removed) = (0, 0);
    for round in 0..STRESS_ROUNDS {
        let (taken_first, removed_first) = stress(round);
        taken += taken_first;
        removed += removed_first;
    }
    // Tickets were dropped both before and after a loop took their items.
    assert!(taken > 0 && removed > 0, "{taken} taken, {removed} removed");
}

/// How many runs of [`stress`] the test makes, of 150 bursts each.
const STRESS_ROUNDS: u64 = 10;

/// The limit of the queue of a [`stress`] run: fewer than its pushers push at once.
const STRESS_LIMIT: usize = 3;

#[test]
fn one_pusher_sees_its_items_taken_in_order() {
    let queue = Queue::<u32>::new(ROOMY);
    let worker = queue.worker();
    let pusher = {
        thread::spawn(move || {
            let tickets: Vec<_> = (0..5_000).map(|i| queue.push_now(i)).collect();
            queue.close();
            tickets
        })
    };
    let taken = run_loop(&worker);
    drop(pusher.join().unwrap());
    assert_eq!(taken, (0..5_000).collect::<Vec<u32>>());
}
