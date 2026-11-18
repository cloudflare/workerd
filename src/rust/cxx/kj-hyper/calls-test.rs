use std::cell::Cell;
use std::future::poll_fn;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::task::Poll;
use std::task::Wake;

use super::*;

/// The task's waker: counts its wakes.
#[derive(Default)]
struct Task(AtomicUsize);

impl Wake for Task {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.0.fetch_add(1, Ordering::Relaxed);
    }
}

/// A call's state, as the test sees it.
#[derive(Default)]
struct Probe {
    /// Once set, the call finishes when next polled.
    done: Cell<bool>,
    /// The call's waker, from its last poll.
    parked: Cell<Option<Waker>>,
    polls: Cell<usize>,
}

impl Probe {
    fn wake(&self) {
        self.parked.take().unwrap().wake();
    }
}

fn call(probe: &Rc<Probe>) -> impl Future<Output = ()> + use<> {
    let probe = Rc::clone(probe);
    poll_fn(move |cx| {
        probe.polls.set(probe.polls.get() + 1);
        if probe.done.get() {
            return Poll::Ready(());
        }
        probe.parked.set(Some(cx.waker().clone()));
        Poll::Pending
    })
}

/// One turn of the task: begins it, polls the calls, and ends it.
fn turn<F: Future<Output = ()>>(wakes: &Wakes, calls: &mut Calls<F>, cx: &Context<'_>) {
    calls.begin(wakes.begin(cx));
    calls.poll();
    wakes.end(cx);
}

fn polls(probes: &[Rc<Probe>]) -> Vec<usize> {
    probes.iter().map(|probe| probe.polls.get()).collect()
}

#[test]
fn a_call_is_polled_when_pushed_and_when_woken_and_its_slot_is_reused() {
    let task = Arc::new(Task::default());
    let task_waker = Waker::from(Arc::clone(&task));
    let cx = Context::from_waker(&task_waker);
    let wakes = Wakes::new();
    let mut calls = Calls::new(&wakes);
    turn(&wakes, &mut calls, &cx);
    // A call pushed during a turn gets its first poll in that turn.
    let first = Rc::new(Probe::default());
    calls.begin(wakes.begin(&cx));
    calls.push(call(&first));
    calls.poll();
    wakes.end(&cx);
    assert_eq!(first.polls.get(), 1);
    assert_eq!(task.0.load(Ordering::Relaxed), 0);

    // Woken between turns, it wakes the task and is polled in the next; done, its slot empties.
    first.done.set(true);
    first.wake();
    assert_eq!(task.0.load(Ordering::Relaxed), 1);
    turn(&wakes, &mut calls, &cx);
    assert_eq!(first.polls.get(), 2);
    assert!(calls.is_empty());

    // The next call takes the same slot, and so the same box and waker.
    let second = Rc::new(Probe::default());
    calls.push(call(&second));
    assert_eq!(calls.slots.len(), 1);
    turn(&wakes, &mut calls, &cx);
    assert_eq!(second.polls.get(), 1);
    calls.clear();
    assert!(calls.is_empty());
}

#[test]
fn only_the_calls_woken_are_polled() {
    let task_waker = Waker::from(Arc::new(Task::default()));
    let cx = Context::from_waker(&task_waker);
    let wakes = Wakes::new();
    let mut calls = Calls::new(&wakes);
    let probes: Vec<_> = (0..3).map(|_| Rc::new(Probe::default())).collect();
    for probe in &probes {
        calls.push(call(probe));
    }
    turn(&wakes, &mut calls, &cx);
    assert_eq!(polls(&probes), [1, 1, 1]);
    probes[1].wake();
    turn(&wakes, &mut calls, &cx);
    assert_eq!(polls(&probes), [1, 2, 1]);
}

#[test]
fn a_call_that_wakes_itself_is_polled_again_in_the_next_turn() {
    let task = Arc::new(Task::default());
    let task_waker = Waker::from(Arc::clone(&task));
    let cx = Context::from_waker(&task_waker);
    let wakes = Wakes::new();
    let mut calls = Calls::new(&wakes);
    calls.push(poll_fn(|cx| {
        cx.waker().wake_by_ref();
        Poll::<()>::Pending
    }));
    turn(&wakes, &mut calls, &cx);
    // Polled once, and the task woken for the wake left untaken.
    assert_eq!(task.0.load(Ordering::Relaxed), 1);
    turn(&wakes, &mut calls, &cx);
    assert_eq!(task.0.load(Ordering::Relaxed), 2);
}

#[test]
fn calls_past_the_bits_share_the_last() {
    let task_waker = Waker::from(Arc::new(Task::default()));
    let cx = Context::from_waker(&task_waker);
    let wakes = Wakes::new();
    let mut calls = Calls::new(&wakes);
    let count = (SHARED - FIRST + 3) as usize;
    let probes: Vec<_> = (0..count).map(|_| Rc::new(Probe::default())).collect();
    for probe in &probes {
        calls.push(call(probe));
    }
    turn(&wakes, &mut calls, &cx);
    assert!(polls(&probes).iter().all(|&n| n == 1));
    // Waking the last polls every call that shares its bit, and only those.
    probes[count - 1].wake();
    turn(&wakes, &mut calls, &cx);
    let twice = polls(&probes).iter().filter(|&&n| n == 2).count();
    assert_eq!(twice, 3);
}
