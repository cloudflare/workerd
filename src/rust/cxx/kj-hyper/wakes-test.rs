use std::sync::atomic::AtomicUsize;

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

fn task() -> (Arc<Task>, Waker) {
    let task = Arc::new(Task::default());
    let task_waker = Waker::from(Arc::clone(&task));
    (task, task_waker)
}

#[test]
fn every_future_counts_as_woken_at_first() {
    let (_, task_waker) = task();
    let wakes = Wakes::new();
    let cx = Context::from_waker(&task_waker);
    assert_eq!(wakes.begin(&cx), !POLLING);
    wakes.end(&cx);
    assert_eq!(wakes.begin(&cx), 0);
}

#[test]
fn a_wake_between_polls_wakes_the_task_and_names_the_future() {
    let (task, task_waker) = task();
    let wakes = Wakes::new();
    let a = wakes.waker(1);
    let b = wakes.waker(2);
    let cx = Context::from_waker(&task_waker);
    wakes.begin(&cx);
    wakes.end(&cx);
    b.wake_by_ref();
    assert_eq!(task.0.load(Ordering::Relaxed), 1);
    assert_eq!(wakes.begin(&cx), 2);
    wakes.end(&cx);
    drop(a);
}

#[test]
fn a_wake_during_a_poll_is_taken_without_waking_the_task() {
    let (task, task_waker) = task();
    let wakes = Wakes::new();
    let a = wakes.waker(1);
    let cx = Context::from_waker(&task_waker);
    wakes.begin(&cx);
    a.wake_by_ref();
    assert!(wakes.take(1));
    assert!(!wakes.take(1));
    wakes.end(&cx);
    assert_eq!(task.0.load(Ordering::Relaxed), 0);
}

#[test]
fn a_wake_during_a_poll_left_untaken_polls_the_task_again() {
    let (task, task_waker) = task();
    let wakes = Wakes::new();
    let a = wakes.waker(1);
    let cx = Context::from_waker(&task_waker);
    wakes.begin(&cx);
    a.wake_by_ref();
    wakes.end(&cx);
    assert_eq!(task.0.load(Ordering::Relaxed), 1);
    assert_eq!(wakes.begin(&cx), 1);
}
