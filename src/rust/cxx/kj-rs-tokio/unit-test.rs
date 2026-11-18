use std::cell::Cell;
use std::collections::HashSet;
use std::rc::Rc;
use std::sync::atomic::AtomicUsize;

use tokio::runtime::Builder;
use tokio::runtime::Runtime;
use tokio::sync::oneshot;

use super::*;

fn runtime(workers: usize) -> Runtime {
    Builder::new_multi_thread()
        .worker_threads(workers)
        .enable_all()
        .build()
        .unwrap()
}

/// Sets its flag when dropped: something a unit's future owns.
struct SetOnDrop(Arc<AtomicBool>);

impl Drop for SetOnDrop {
    fn drop(&mut self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

#[test]
fn a_unit_resolves_to_its_future_s_output() {
    let runtime = runtime(2);
    let output = runtime.block_on(async {
        spawn_unit(LoopUnit::new((), |_| async {
            // Not `Send`, and held across an await.
            let count = Rc::new(Cell::new(0));
            for _ in 0..3 {
                tokio::time::sleep(Duration::from_millis(1)).await;
                count.set(count.get() + 1);
            }
            count.get()
        }))
        .await
        .unwrap()
    });
    assert_eq!(output, Ok(3));
}

#[test]
fn a_wake_from_another_thread_resumes_the_unit() {
    let runtime = runtime(2);
    let (send, receive) = oneshot::channel();
    let unit = runtime.spawn(tokio::task::coop::unconstrained(LoopUnit::new(
        (),
        |_| async move { receive.await.unwrap() },
    )));
    let sender = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(20));
        send.send(7).unwrap();
    });
    assert_eq!(runtime.block_on(unit).unwrap(), Ok(7));
    sender.join().unwrap();
}

#[test]
fn a_unit_is_polled_by_more_than_one_thread() {
    // More units than workers, each waking up many times: unless every unit happens to be
    // picked up by the same worker every time, some unit sees a second thread.
    let runtime = runtime(4);
    let migrated = Arc::new(AtomicUsize::new(0));
    runtime.block_on(async {
        let units: Vec<_> = (0..16)
            .map(|_| {
                let migrated = Arc::clone(&migrated);
                spawn_unit(LoopUnit::new((), move |_| async move {
                    let mut threads = HashSet::new();
                    // Something of the loop that every poll touches.
                    let turns = Rc::new(Cell::new(0_u32));
                    for _ in 0..200 {
                        threads.insert(std::thread::current().id());
                        turns.set(turns.get() + 1);
                        tokio::time::sleep(Duration::from_micros(200)).await;
                    }
                    if threads.len() > 1 {
                        migrated.fetch_add(1, Ordering::SeqCst);
                    }
                    turns.get()
                }))
            })
            .collect();
        for unit in units {
            assert_eq!(unit.await.unwrap(), Ok(200));
        }
    });
    assert!(migrated.load(Ordering::SeqCst) > 0);
}

#[test]
fn units_talk_to_each_other_through_channels() {
    let runtime = runtime(3);
    runtime.block_on(async {
        let (to_echo, mut requests) =
            tokio::sync::mpsc::unbounded_channel::<(u32, oneshot::Sender<u32>)>();
        let echo = spawn_unit(LoopUnit::new((), move |_| async move {
            let mut served = 0;
            while let Some((value, reply)) = requests.recv().await {
                reply.send(value + 1).unwrap();
                served += 1;
            }
            served
        }));
        let callers: Vec<_> = (0..8)
            .map(|caller| {
                let to_echo = to_echo.clone();
                spawn_unit(LoopUnit::new((), move |_| async move {
                    let mut sum = 0;
                    for i in 0..100 {
                        let (reply, replied) = oneshot::channel();
                        to_echo.send((caller * 1000 + i, reply)).unwrap();
                        sum += replied.await.unwrap();
                    }
                    sum
                }))
            })
            .collect();
        drop(to_echo);
        for (caller, unit) in (0..).zip(callers) {
            let expected: u32 = (0..100).map(|i| caller * 1000 + i + 1).sum();
            assert_eq!(unit.await.unwrap(), Ok(expected));
        }
        assert_eq!(echo.await.unwrap(), Ok(800));
    });
}

#[test]
fn the_runtime_is_the_loop_s_while_a_unit_is_entered() {
    let runtime = runtime(2);
    let id = runtime.handle().id();
    let inside = runtime.block_on(async {
        // This task is not a unit.
        assert!(crate::current_handle().is_none());
        spawn_unit(LoopUnit::new((), |_| async {
            tokio::time::sleep(Duration::from_millis(1)).await;
            crate::current_handle().map(|handle| handle.id())
        }))
        .await
        .unwrap()
    });
    assert_eq!(inside, Ok(Some(id)));
}

#[test]
fn dropping_a_pending_unit_destroys_what_is_on_its_loop() {
    let runtime = runtime(2);
    let dropped = Arc::new(AtomicBool::new(false));
    let (started, has_started) = oneshot::channel();
    let unit = runtime.spawn(tokio::task::coop::unconstrained(LoopUnit::new((), {
        let dropped = Arc::clone(&dropped);
        move |_| async move {
            let _owned = SetOnDrop(dropped);
            started.send(()).unwrap();
            std::future::pending::<()>().await;
        }
    })));
    runtime.block_on(has_started).unwrap();
    assert!(!dropped.load(Ordering::SeqCst));
    unit.abort();
    assert!(runtime.block_on(unit).unwrap_err().is_cancelled());
    assert!(dropped.load(Ordering::SeqCst));
}

#[test]
fn a_unit_that_completes_destroys_its_future_first() {
    let runtime = runtime(2);
    let dropped = Arc::new(AtomicBool::new(false));
    let seen = runtime.block_on(async {
        let unit = spawn_unit(LoopUnit::new((), {
            let dropped = Arc::clone(&dropped);
            move |_| async move {
                let _owned = SetOnDrop(dropped);
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        }));
        unit.await.unwrap().unwrap();
        dropped.load(Ordering::SeqCst)
    });
    assert!(seen);
}

#[test]
fn a_unit_needs_a_multi_thread_runtime() {
    let runtime = Builder::new_current_thread().enable_all().build().unwrap();
    let result = runtime.block_on(LoopUnit::new((), |_| async {}));
    assert!(result.is_err());
}

/// Counts the entries of the threads that drive a unit.
struct CountingScope(Arc<AtomicUsize>);

impl UnitScope for CountingScope {
    fn enter(&self, body: &mut dyn FnMut()) {
        self.0.fetch_add(1, Ordering::SeqCst);
        body();
    }
}

#[test]
fn every_poll_enters_the_unit_s_scope() {
    let runtime = runtime(2);
    let entries = Arc::new(AtomicUsize::new(0));
    runtime.block_on(async {
        let scope = CountingScope(Arc::clone(&entries));
        spawn_unit(LoopUnit::new(scope, |_| async {
            for _ in 0..3 {
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        }))
        .await
        .unwrap()
        .unwrap();
    });
    // The first poll and one for each sleep, at least.
    assert!(entries.load(Ordering::SeqCst) >= 4);
}
