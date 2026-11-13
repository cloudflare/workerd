// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests of `jsg::ExternalMemoryAdjustment` and `jsg::ExternalMemoryTarget`: what they report to
//! V8, with and without the isolate lock, and after the isolate is gone.
//!
//! An adjustment of 32 GiB or more, and the rise that `set()` caps at `SSIZE_MAX`, abort the
//! process, so no test here makes one.

use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::sync::mpsc;

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

const fn assert_send_sync<T: Send + Sync>() {}
const _: () = assert_send_sync::<jsg::ExternalMemoryAdjustment>();
const _: () = assert_send_sync::<jsg::ExternalMemoryTarget>();

#[test]
fn adjustment_under_the_lock_reaches_v8_at_once() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let base = crate::Harness::external_memory(lock);
        let target = lock.external_memory_target();

        let mut adjustment = lock.external_memory_adjustment(1000);
        assert_eq!(adjustment.amount(), 1000);
        assert_eq!(crate::Harness::external_memory(lock), base + 1000);

        adjustment.adjust(24).unwrap();
        assert_eq!(adjustment.amount(), 1024);
        assert_eq!(crate::Harness::external_memory(lock), base + 1024);

        adjustment.adjust(-512).unwrap();
        assert_eq!(adjustment.amount(), 512);
        assert_eq!(crate::Harness::external_memory(lock), base + 512);

        adjustment.set(0);
        assert_eq!(adjustment.amount(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);

        adjustment.set(100);
        assert_eq!(adjustment.amount(), 100);
        assert_eq!(crate::Harness::external_memory(lock), base + 100);

        // A target's adjustment made under the lock reaches V8 at once too.
        let other = target.adjustment(50);
        assert_eq!(crate::Harness::external_memory(lock), base + 150);
        assert_eq!(target.pending_update_for_test(), 0);

        drop(other);
        drop(adjustment);
        assert_eq!(crate::Harness::external_memory(lock), base);
        assert_eq!(target.pending_update_for_test(), 0);
        Ok(())
    });
}

#[test]
fn adjusting_below_zero_fails_and_keeps_the_amount() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let base = crate::Harness::external_memory(lock);
        let mut adjustment = lock.external_memory_adjustment(50);

        // C++ adjust() makes the check and throws. The error is internal, as C++'s is, which
        // `lowering_below_zero_in_a_method_throws_a_catchable_internal_error` checks from
        // JavaScript.
        let err = adjustment
            .adjust(-200)
            .expect_err("lowering an adjustment below zero must fail");
        assert_eq!(err.name, jsg::ExceptionType::Error);
        assert!(
            err.message
                .contains("Memory usage may not be decreased below zero"),
            "{}",
            err.message
        );

        assert_eq!(adjustment.amount(), 50);
        assert_eq!(crate::Harness::external_memory(lock), base + 50);
        adjustment.adjust(-50).unwrap();
        assert_eq!(adjustment.amount(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

#[test]
fn adjustment_without_the_lock_reaches_v8_at_the_next_lock() {
    let harness = crate::Harness::new();
    let mut base = 0;
    let mut target = None;
    let mut kept = None;

    harness.run_in_context(|lock, _ctx| {
        base = crate::Harness::external_memory(lock);
        let t = lock.external_memory_target();

        // Another thread has not locked the isolate, so its changes wait for the next lock.
        let adjustment = std::thread::scope(|scope| {
            scope
                .spawn(|| {
                    let adjustment = t.adjustment(1000);
                    assert_eq!(t.pending_update_for_test(), 1000);

                    // Made and dropped before the next lock: it cancels out.
                    let mut dropped = t.adjustment(1000);
                    dropped.adjust(500).unwrap();
                    assert_eq!(t.pending_update_for_test(), 2500);
                    drop(dropped);
                    assert_eq!(t.pending_update_for_test(), 1000);
                    adjustment
                })
                .join()
                .unwrap()
        });

        assert_eq!(adjustment.amount(), 1000);
        assert_eq!(t.pending_update_for_test(), 1000);
        assert_eq!(crate::Harness::external_memory(lock), base);
        kept = Some(adjustment);
        target = Some(t);
        Ok(())
    });

    let target = target.unwrap();
    harness.run_in_context(|lock, _ctx| {
        // Locking the isolate handed the pending total to V8.
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base + 1000);

        // Under the lock, the adjustment that another thread made reports at once.
        let mut adjustment = kept.take().unwrap();
        adjustment.adjust(-400).unwrap();
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base + 600);
        kept = Some(adjustment);
        Ok(())
    });

    // Dropped without the lock, the release waits for the next lock too.
    std::thread::spawn(move || drop(kept)).join().unwrap();
    assert_eq!(target.pending_update_for_test(), -600);

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

#[test]
fn adjustment_can_outlive_its_isolate() {
    let mut target = None;
    let mut adjustment = None;
    {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let t = lock.external_memory_target();
            assert!(t.is_isolate_alive());
            adjustment = Some(t.adjustment(100));
            target = Some(t);
            Ok(())
        });
    }

    let target = target.unwrap();
    assert!(!target.is_isolate_alive());

    // Changing, making and dropping adjustments of a destroyed isolate is safe: the changes go to
    // the pending total, which no lock hands to V8 any more.
    assert_eq!(target.pending_update_for_test(), 0);
    let mut adjustment = adjustment.unwrap();
    adjustment.adjust(100).unwrap();
    assert_eq!(adjustment.amount(), 200);
    assert_eq!(target.pending_update_for_test(), 100);
    let other = target.adjustment(10);
    assert_eq!(target.pending_update_for_test(), 110);
    drop(other);
    drop(adjustment);
    assert_eq!(target.pending_update_for_test(), -100);
}

#[test]
fn adjustment_under_another_isolates_lock_waits_for_its_own() {
    let harness = crate::Harness::new();
    let other_harness = crate::Harness::new();
    let mut base = 0;
    let mut target = None;
    harness.run_in_context(|lock, _ctx| {
        base = crate::Harness::external_memory(lock);
        target = Some(lock.external_memory_target());
        Ok(())
    });
    let target = target.unwrap();

    let mut kept = None;
    other_harness.run_in_context(|lock, _ctx| {
        let other_base = crate::Harness::external_memory(lock);
        let mut adjustment = target.adjustment(100);
        adjustment.adjust(50).unwrap();
        assert_eq!(target.pending_update_for_test(), 150);
        assert_eq!(crate::Harness::external_memory(lock), other_base);
        kept = Some(adjustment);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base + 150);
        drop(kept.take());
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

#[test]
fn adjustment_dropped_under_another_isolates_lock_waits_for_its_own() {
    let harness = crate::Harness::new();
    let other_harness = crate::Harness::new();
    let mut base = 0;
    let mut kept = None;
    let mut target = None;
    harness.run_in_context(|lock, _ctx| {
        base = crate::Harness::external_memory(lock);
        kept = Some(lock.external_memory_adjustment(100));
        target = Some(lock.external_memory_target());
        Ok(())
    });
    let target = target.unwrap();

    other_harness.run_in_context(|lock, _ctx| {
        let other_base = crate::Harness::external_memory(lock);
        drop(kept.take());
        assert_eq!(target.pending_update_for_test(), -100);
        assert_eq!(crate::Harness::external_memory(lock), other_base);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

// V8 aborts the process if its total falls below zero. An adjustment made on another thread is
// pending, so releasing it under the lock before the next one hands the pending total to V8 must
// not take V8's total below where it was. 1 GiB is far more than the isolate reports, so V8 would
// see the fall.
#[test]
fn adjustment_made_without_the_lock_can_be_released_under_it_at_once() {
    const AMOUNT: usize = 1 << 30;
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let base = crate::Harness::external_memory(lock);
        let target = lock.external_memory_target();
        let make_pending = || {
            let adjustment =
                std::thread::scope(|scope| scope.spawn(|| target.adjustment(AMOUNT)).join());
            assert_eq!(target.pending_update_for_test(), 1 << 30);
            adjustment.unwrap()
        };

        drop(make_pending());
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);

        let mut lowered = make_pending();
        lowered.adjust(-(1 << 30)).unwrap();
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        drop(lowered);

        let mut set = make_pending();
        set.set(0);
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        drop(set);
        Ok(())
    });
}

// A decrease under the lock nets the pending total into what it reports. When the pending
// increases exceed it, V8 gets nothing, so that a decrease (often in a destructor or a GC
// finalizer) never makes V8 start a GC, and the net rise waits for the next lock.
#[test]
fn decrease_under_the_lock_leaves_a_larger_pending_increase_pending() {
    let harness = crate::Harness::new();
    let mut base = 0;
    let mut target = None;
    let mut kept = None;
    harness.run_in_context(|lock, _ctx| {
        base = crate::Harness::external_memory(lock);
        let t = lock.external_memory_target();
        let (big, small) = std::thread::scope(|scope| {
            scope
                .spawn(|| (t.adjustment(2000), t.adjustment(500)))
                .join()
        })
        .unwrap();
        assert_eq!(t.pending_update_for_test(), 2500);

        drop(small);
        assert_eq!(t.pending_update_for_test(), 2000);
        assert_eq!(crate::Harness::external_memory(lock), base);
        kept = Some(big);
        target = Some(t);
        Ok(())
    });

    let target = target.unwrap();
    harness.run_in_context(|lock, _ctx| {
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base + 2000);
        drop(kept.take());
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

// A pending decrease is netted in too: the decrease V8 gets is the sum of both.
#[test]
fn decrease_under_the_lock_reports_a_pending_decrease_with_it() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let base = crate::Harness::external_memory(lock);
        let target = lock.external_memory_target();
        let released = lock.external_memory_adjustment(300);
        let mut lowered = lock.external_memory_adjustment(200);
        assert_eq!(crate::Harness::external_memory(lock), base + 500);

        std::thread::scope(|scope| scope.spawn(|| drop(released)).join()).unwrap();
        assert_eq!(target.pending_update_for_test(), -300);
        assert_eq!(crate::Harness::external_memory(lock), base + 500);

        lowered.adjust(-200).unwrap();
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

// Threads make and drop adjustments without the lock while the thread holding it drops others
// that they hand it. V8's total must never fall below zero, which V8 checks and aborts on, and
// once every adjustment is gone it is back where it started. 1 MiB each is far more than the
// isolate reports, so a decrease reaching V8 before its increase would take the total below zero.
#[test]
fn concurrent_adjustments_with_and_without_the_lock_balance_out() {
    const THREADS: usize = 4;
    const ROUNDS: usize = 200;
    const AMOUNT: usize = 1 << 20;
    let harness = crate::Harness::new();
    let mut base = 0;
    let mut target = None;
    harness.run_in_context(|lock, _ctx| {
        base = crate::Harness::external_memory(lock);
        let t = lock.external_memory_target();
        let (sender, receiver) = mpsc::channel::<jsg::ExternalMemoryAdjustment>();
        std::thread::scope(|scope| {
            for _ in 0..THREADS {
                let sender = sender.clone();
                let t = &t;
                scope.spawn(move || {
                    for round in 0..ROUNDS {
                        let mut mine = t.adjustment(AMOUNT);
                        let handed = t.adjustment(AMOUNT);
                        mine.adjust(isize::try_from(round).unwrap()).unwrap();
                        sender.send(handed).unwrap();
                        drop(mine);
                    }
                });
            }
            drop(sender);
            // The lock holder drops what it is handed, under the lock, as it arrives.
            for adjustment in receiver {
                drop(adjustment);
            }
        });
        target = Some(t);
        Ok(())
    });

    let target = target.unwrap();
    harness.run_in_context(|lock, _ctx| {
        assert_eq!(target.pending_update_for_test(), 0);
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}

#[test]
fn cloned_target_is_the_same_target() {
    let harness = crate::Harness::new();
    let mut target = None;
    harness.run_in_context(|lock, _ctx| {
        target = Some(lock.external_memory_target());
        Ok(())
    });
    let target = target.unwrap();
    let clone = target.clone();

    let adjustment = std::thread::spawn(move || clone.adjustment(30))
        .join()
        .unwrap();
    assert_eq!(target.pending_update_for_test(), 30);

    harness.run_in_context(|lock, _ctx| {
        let other = lock.external_memory_target();
        assert_eq!(other.pending_update_for_test(), 0);
        drop(adjustment);
        assert_eq!(target.pending_update_for_test(), 0);
        Ok(())
    });
}

/// A resource that owns memory outside the V8 heap and reports it, as C++ resources do.
#[jsg_resource(memory_info)]
struct ExternalBuffer {
    data: RefCell<Vec<u8>>,
    external_memory: RefCell<jsg::ExternalMemoryAdjustment>,
}

#[jsg_resource]
impl ExternalBuffer {
    #[jsg_method]
    fn grow(&self, bytes: u32) {
        let mut data = self.data.borrow_mut();
        let len = data.len() + bytes as usize;
        data.resize(len, 0);
        self.external_memory.borrow_mut().set(len);
    }

    /// Lowers the adjustment by `bytes` without freeing the data, so that lowering it below zero
    /// can be tested.
    #[jsg_method]
    fn shrink_accounting(&self, bytes: u32) -> jsg::Result<()> {
        self.external_memory
            .borrow_mut()
            .adjust(-isize::try_from(bytes).unwrap())
    }
}

#[test]
fn resource_releases_its_external_memory_when_collected() {
    let harness = crate::Harness::new();

    harness.run_in_context(|lock, ctx| {
        let base = crate::Harness::external_memory(lock);
        let data = vec![0; 4096];
        let external_memory = lock.external_memory_adjustment(data.len());
        let buffer = jsg::Rc::new(
            lock,
            ExternalBuffer {
                data: RefCell::new(data),
                external_memory: RefCell::new(external_memory),
            },
        );
        assert_eq!(crate::Harness::external_memory(lock), base + 4096);

        ctx.set_global(lock, "buffer", buffer);
        ctx.eval_raw(lock, "buffer.grow(1024)").unwrap();
        assert_eq!(crate::Harness::external_memory(lock), base + 5120);

        // The resource reports its data but nothing for the adjustment in a heap snapshot.
        let snapshot = crate::tests::memory_info::Snapshot::take(lock);
        let node = snapshot.find("ExternalBuffer");
        let edges = snapshot.edge_names(node);
        assert!(edges.contains(&"data"), "{edges:?}");
        assert!(!edges.contains(&"external_memory"), "{edges:?}");

        ctx.eval_raw(lock, "buffer = undefined").unwrap();
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        // Nothing else in this isolate holds external memory that the GC frees, so the total falls
        // by exactly what the resource's adjustment held.
        let before = crate::Harness::external_memory(lock);
        crate::Harness::request_gc(lock);
        assert_eq!(before - crate::Harness::external_memory(lock), 5120);
        Ok(())
    });
}

/// A resource holding an adjustment that counts its drops, to tell a release at teardown from no
/// release at all.
#[jsg_resource]
struct TeardownBuffer {
    _external_memory: RefCell<jsg::ExternalMemoryAdjustment>,
}

#[jsg_resource]
impl TeardownBuffer {}

static TEARDOWN_BUFFER_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for TeardownBuffer {
    fn drop(&mut self) {
        TEARDOWN_BUFFER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn resource_alive_at_teardown_releases_its_external_memory_before_v8_is_gone() {
    TEARDOWN_BUFFER_DROPS.store(0, Ordering::SeqCst);
    let mut target = None;
    {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, ctx| {
            let external_memory = lock.external_memory_adjustment(4096);
            let buffer = jsg::Rc::new(
                lock,
                TeardownBuffer {
                    _external_memory: RefCell::new(external_memory),
                },
            );
            ctx.set_global(lock, "buffer", buffer);
            target = Some(lock.external_memory_target());
            Ok(())
        });
        assert_eq!(TEARDOWN_BUFFER_DROPS.load(Ordering::SeqCst), 0);
    }

    // Teardown dropped the resource under the lock, while the isolate was still its target, so the
    // release reached V8 rather than the pending total.
    assert_eq!(TEARDOWN_BUFFER_DROPS.load(Ordering::SeqCst), 1);
    let target = target.unwrap();
    assert!(!target.is_isolate_alive());
    assert_eq!(target.pending_update_for_test(), 0);
}

#[test]
fn lowering_below_zero_in_a_method_throws_a_catchable_internal_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let base = crate::Harness::external_memory(lock);
        let external_memory = lock.external_memory_adjustment(100);
        let buffer = jsg::Rc::new(
            lock,
            ExternalBuffer {
                data: RefCell::new(vec![0; 100]),
                external_memory: RefCell::new(external_memory),
            },
        );
        ctx.set_global(lock, "buffer", buffer);

        // The script catches the error and runs on, as it does when a C++ method throws.
        let message = ctx.eval::<String>(
            lock,
            "try { buffer.shrinkAccounting(1000); 'nothing thrown' } catch (e) { e.message }",
        );
        assert!(
            message
                .as_ref()
                .is_ok_and(|m| m.starts_with("internal error; reference = ")),
            "{message:?}"
        );
        assert_eq!(crate::Harness::external_memory(lock), base + 100);

        ctx.eval_raw(lock, "buffer.shrinkAccounting(100)").unwrap();
        assert_eq!(crate::Harness::external_memory(lock), base);
        Ok(())
    });
}
