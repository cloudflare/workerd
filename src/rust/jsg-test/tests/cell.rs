// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `jsg::Cell` and `jsg::RefCell`, which make a reference strong before it leaves
//! the cell.
//!
//! Once GC has visited a `jsg::Rc` or a `TracedGlobal` in a field of a resource that has a
//! wrapper, the reference is weak and depends on the resource's trace. Moved out of the field
//! by a bitwise move, it would be neither strong nor traced, and the next GC would collect the
//! wrapper of the `jsg::Rc`'s resource, or the value of the `TracedGlobal`. These tests take
//! references out through the cells after such a visit, run a GC, and check that nothing was
//! lost.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

static TASK_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A resource that JavaScript creates and hands to a `Scheduler`, which then holds the only
/// reference to it.
#[jsg_resource]
struct Task {
    /// A value that only this task keeps alive, once GC has put the handle in traced mode.
    payload: jsg::Cell<Option<v8::TracedGlobal<v8::Value>>>,
}

impl Drop for Task {
    fn drop(&mut self) {
        TASK_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Task {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self {
            payload: jsg::Cell::new(None),
        }
    }

    #[jsg_method]
    fn set_payload<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        self.payload.set(Some(value.to_traced_global(lock)));
    }
}

/// A resource that holds `jsg::Rc`s of tasks in both kinds of cell.
#[jsg_resource]
struct Scheduler {
    pending: jsg::Cell<Option<jsg::Rc<Task>>>,
    queue: jsg::RefCell<Vec<jsg::Rc<Task>>>,
}

#[jsg_resource]
impl Scheduler {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self {
            pending: jsg::Cell::new(None),
            queue: jsg::RefCell::new(Vec::new()),
        }
    }

    #[jsg_method]
    fn schedule(&self, task: jsg::Rc<Task>) {
        self.pending.set(Some(task));
    }

    #[jsg_method]
    fn enqueue(&self, lock: &mut jsg::Lock<'_>, task: jsg::Rc<Task>) {
        self.queue.borrow_mut(lock).push(task);
    }
}

/// Creates the global `scheduler` with one scheduled and one queued task, which JavaScript
/// creates, marks with an own property of their wrappers and gives a payload, and then runs a GC.
/// The scheduler's wrapper is reachable, so the visit makes the `jsg::Rc`s in its fields weak,
/// and the tasks' `TracedGlobal`s traced. The scripts end in `undefined` so that no `Local` of a
/// task outlives them.
fn setup(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) -> jsg::Rc<Scheduler> {
    let task = jsg::resource::function_template_of::<Task>(lock);
    ctx.set_global(lock, "Task", task);
    let scheduler = jsg::resource::function_template_of::<Scheduler>(lock);
    ctx.set_global(lock, "Scheduler", scheduler);
    ctx.eval_raw(
        lock,
        r"
        globalThis.scheduler = new Scheduler();
        for (const [name, add] of [['scheduled', 'schedule'], ['queued', 'enqueue']]) {
            const task = new Task();
            task.mark = name;
            task.setPayload({ of: name });
            scheduler[add](task);
        }
        undefined
        ",
    )
    .expect("should evaluate");
    let scheduler: jsg::Rc<Scheduler> = ctx.eval(lock, "scheduler").expect("should evaluate");
    crate::Harness::request_gc(lock);
    scheduler
}

/// The own property `mark` of the wrapper of `task`, which is `undefined` if the wrapper is a
/// new one.
fn mark<'s>(lock: &mut jsg::Lock<'s>, task: &jsg::Rc<Task>) -> v8::Local<'s, v8::Value> {
    let wrapper = v8::Local::<v8::Object>::try_from(task.clone().to_js(lock)).unwrap();
    wrapper.get(lock, "mark").unwrap()
}

fn assert_marked(lock: &mut jsg::Lock<'_>, task: &jsg::Rc<Task>, expected: &str) {
    let mark = mark(lock, task);
    assert!(mark.is_string(), "the wrapper was replaced: {mark:?}");
    assert_eq!(String::from_js(lock, mark).unwrap(), expected);
}

/// A `jsg::Rc` taken out of a `jsg::Cell` after a visit made it weak is strong, so a GC leaves
/// its resource's wrapper alone.
#[test]
fn rc_taken_from_a_cell_keeps_its_wrapper() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let task = scheduler.pending.take(lock).expect("scheduled above");
        crate::Harness::request_gc(lock);
        assert_marked(lock, &task, "scheduled");
        Ok(())
    });
}

/// `replace` makes the old value strong as `take` does.
#[test]
fn rc_replaced_in_a_cell_keeps_its_wrapper() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let task = scheduler
            .pending
            .replace(lock, None)
            .expect("scheduled above");
        crate::Harness::request_gc(lock);
        assert_marked(lock, &task, "scheduled");
        Ok(())
    });
}

/// `jsg::RefCell::borrow_mut` makes everything in the cell strong, so what the borrower pops out
/// is strong.
#[test]
fn rc_popped_from_a_ref_cell_keeps_its_wrapper() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let task = scheduler
            .queue
            .borrow_mut(lock)
            .pop()
            .expect("queued above");
        crate::Harness::request_gc(lock);
        assert_marked(lock, &task, "queued");
        Ok(())
    });
}

/// With its wrapper kept, the resource's own fields stay traced: its `TracedGlobal` keeps its
/// value.
#[test]
fn rc_taken_from_a_cell_keeps_the_values_its_resource_traces() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let task = scheduler.pending.take(lock).expect("scheduled above");
        crate::Harness::request_gc(lock);
        let payload = task.payload.take(lock).expect("set above");
        let payload = v8::Local::<v8::Object>::try_from(payload.as_local(lock)).unwrap();
        let of = payload.get(lock, "of").unwrap();
        assert_eq!(String::from_js(lock, of).unwrap(), "scheduled");
        Ok(())
    });
}

/// A `TracedGlobal` taken out of a `jsg::Cell` after a visit put it in traced mode is strong
/// again, so a GC leaves its value alone even though nothing traces the handle.
#[test]
fn traced_global_taken_from_a_cell_keeps_its_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        // Taken from the task while the scheduler still holds and traces the task.
        let payload = scheduler.queue.borrow()[0]
            .payload
            .take(lock)
            .expect("set above");
        crate::Harness::request_gc(lock);
        let payload = v8::Local::<v8::Object>::try_from(payload.as_local(lock)).unwrap();
        let of = payload.get(lock, "of").unwrap();
        assert_eq!(String::from_js(lock, of).unwrap(), "queued");
        Ok(())
    });
}

/// A `jsg::Rc` taken out of one resource forgets which resource traced it, so another resource
/// can hold and trace it. (The C++ `Wrappable` asserts that a reference keeps one parent, and
/// aborts the process otherwise.)
#[test]
fn rc_taken_from_one_resource_is_traced_by_another() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let other: jsg::Rc<Scheduler> = ctx
            .eval(lock, "globalThis.other = new Scheduler()")
            .expect("should evaluate");
        let task = scheduler.pending.take(lock).expect("scheduled above");
        other.pending.set(Some(task));
        crate::Harness::request_gc(lock);
        // The second visit finds the reference with its new parent recorded.
        crate::Harness::request_gc(lock);
        let task = other.pending.take(lock).expect("set above");
        assert_marked(lock, &task, "scheduled");
        Ok(())
    });
}

/// A `jsg::Rc` put back after it was taken out goes back to depending on the owner's trace, so
/// the owner and the task are collected once JavaScript lets go of the owner.
#[test]
fn rc_put_back_into_a_cell_is_collected_with_its_owner() {
    TASK_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        let task = scheduler.pending.take(lock).expect("scheduled above");
        scheduler.pending.set(Some(task));
        let task = scheduler
            .queue
            .borrow_mut(lock)
            .pop()
            .expect("queued above");
        scheduler.queue.borrow_mut(lock).push(task);
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 0);

        ctx.eval_raw(lock, "globalThis.scheduler = undefined")
            .expect("should evaluate");
        drop(scheduler);
        Ok(())
    });
    // GC in a separate context so no `Local` of the scheduler's wrapper is left.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// Everything in a mutably borrowed `jsg::RefCell` is strong, so a GC that runs during the
/// borrow, which cannot visit the cell, loses nothing. (A std `RefCell` holding GC references
/// aborts a debug build here.)
#[test]
fn gc_during_a_mutable_borrow_loses_nothing() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let scheduler = setup(lock, ctx);
        {
            let mut queue = scheduler.queue.borrow_mut(lock);
            crate::Harness::request_gc(lock);
            let task = queue.pop().expect("queued above");
            queue.push(task);
        }
        crate::Harness::request_gc(lock);
        let task = scheduler.queue.borrow()[0].clone();
        assert_marked(lock, &task, "queued");
        let payload = task.payload.take(lock).expect("set above");
        assert!(payload.as_local(lock).is_object());
        Ok(())
    });
}
