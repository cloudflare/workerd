// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for dropping a `TracedGlobal` without the isolate lock, on another thread, or after its
//! isolate is gone, and for moving one that GC has visited, in and out of the resource that
//! traces it.

use std::cell::Cell;
use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// A resource whose only job is to own a `TracedGlobal`, so that dropping the resource drops the
/// handle.
#[jsg_resource]
struct HandleHolder {
    handle: Cell<Option<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl HandleHolder {
    /// Stores `value` in a `TracedGlobal`. The `Local` dies with the callback's scope, so no
    /// `Local` keeps the value alive.
    #[jsg_method]
    fn set<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        self.handle.set(Some(value.to_traced_global(lock)));
    }

    /// Returns a new `Counted`, so that scripts can make one that no `Local` outlives.
    #[jsg_method]
    fn new_counted(&self) -> jsg::Rc<Counted> {
        jsg::Rc::new(Counted)
    }
}

static HANDLE_HOLDER_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for HandleHolder {
    fn drop(&mut self) {
        HANDLE_HOLDER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A resource that owns a `HandleHolder`, which GC then reaches only through this resource's
/// trace.
#[jsg_resource]
struct HolderParent {
    child: jsg::Rc<HandleHolder>,
}

#[jsg_resource]
impl HolderParent {
    /// Stores `value` in a `TracedGlobal` of the child. The `Local` dies with the callback's scope,
    /// so no `Local` keeps the value alive.
    #[jsg_method]
    fn set_child_value<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        self.child.handle.set(Some(value.to_traced_global(lock)));
    }
}

/// A resource that collects `TracedGlobal`s in a `Vec`, which moves them whenever it grows.
#[jsg_resource]
struct HandleList {
    handles: RefCell<Vec<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl HandleList {
    #[jsg_method]
    fn push<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        self.handles.borrow_mut().push(value.to_traced_global(lock));
    }
}

static HANDLE_LIST_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for HandleList {
    fn drop(&mut self) {
        HANDLE_LIST_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A resource whose drops the tests count.
#[jsg_resource]
struct Counted;

#[jsg_resource]
impl Counted {}

static COUNTED_DROPS: AtomicUsize = AtomicUsize::new(0);

impl Drop for Counted {
    fn drop(&mut self) {
        COUNTED_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A `TracedGlobal` is `'static`, so it can outlive the harness, and with it the isolate. Dropping
/// it then must leave the handle alone: `v8::Isolate::Dispose` freed it with the isolate.
#[test]
fn global_dropped_after_its_isolate_is_disposed() {
    let harness = crate::Harness::new();
    let mut global: Option<v8::TracedGlobal<v8::Value>> = None;
    harness.run_in_context(|lock, _ctx| {
        global = Some("escaped".to_js(lock).unwrap().to_traced_global(lock));
        Ok(())
    });
    drop(harness);
    drop(global.expect("created in the context"));
}

/// A resource that has a JavaScript wrapper and outlives its isolate is destroyed at once when
/// its last `Rc` goes, since the isolate detached its wrapper at teardown. The `TracedGlobal` it
/// owns is dropped with it, after the isolate was disposed.
#[test]
fn wrapped_resource_dropped_after_its_isolate_is_disposed() {
    HANDLE_HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut holder: Option<jsg::Rc<HandleHolder>> = None;
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        resource
            .handle
            .set(Some("held".to_js(lock).unwrap().to_traced_global(lock)));
        // Wrapping the resource ties its `Wrappable` to the isolate, which the isolate undoes
        // when it is torn down.
        ctx.set_global(lock, "holder", resource.clone());
        holder = Some(resource);
        Ok(())
    });
    drop(harness);
    assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 0);

    drop(holder.expect("created in the context"));
    assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 1);
}

/// A resource that is still wrapped when its isolate is torn down is dropped by the teardown,
/// outside any GC, together with the `TracedGlobal` it owns. GC put that `TracedGlobal` in traced
/// mode while the wrapper was reachable.
#[test]
fn wrapped_resource_in_traced_mode_is_dropped_with_its_isolate() {
    HANDLE_HOLDER_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        let wrapper = resource.clone().to_js(lock)?;
        // The resource holds its own wrapper, and JavaScript holds it too.
        resource.handle.set(Some(wrapper.to_traced_global(lock)));
        ctx.set_global(lock, "holder", wrapper);
        drop(resource);

        // Tracing the wrapper puts the handle in traced mode; the wrapper is reachable from
        // the global object, so the cycle survives.
        crate::Harness::request_gc(lock);
        assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    drop(harness);
    assert_eq!(HANDLE_HOLDER_DROPS.load(Ordering::SeqCst), 1);
}

/// Wraps a new `HandleHolder` and sets its `TracedGlobal` to a value that the global object also
/// reaches, as `globalThis.value`. GC then puts the `TracedGlobal` in traced mode: its strong
/// handle is weak, and a traced node, which the resource's trace marks, keeps the value alive.
///
/// Returns the resource and the size of the handles in use before the `TracedGlobal` was created.
fn holder_in_traced_mode(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
) -> (jsg::Rc<HandleHolder>, usize) {
    let resource = jsg::Rc::new(HandleHolder {
        handle: Cell::new(None),
    });
    ctx.set_global(lock, "holder", resource.clone());
    // No `Local` of the value outlives the scripts, so that once `globalThis.value` lets go of
    // it, only the `TracedGlobal` can keep it alive.
    ctx.eval_raw(lock, "globalThis.value = {}; undefined")
        .expect("should evaluate");
    // Collects whatever the setup left behind, so that only the `TracedGlobal` changes the count.
    crate::Harness::request_gc(lock);
    let baseline = crate::Harness::used_global_handles_size(lock);

    ctx.eval_raw(lock, "holder.set(globalThis.value); undefined")
        .expect("should evaluate");
    crate::Harness::request_gc(lock);
    (resource, baseline)
}

/// Dropping a `TracedGlobal` in traced mode resets its strong handle and lets go of its traced
/// node, which the next full GC frees.
#[test]
fn traced_global_dropped_frees_its_traced_node_at_the_next_gc() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let (resource, baseline) = holder_in_traced_mode(lock, ctx);
        drop(resource.handle.take());
        assert!(
            crate::Harness::used_global_handles_size(lock) > baseline,
            "dropping leaves the traced node for the next full GC",
        );
        crate::Harness::request_gc(lock);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// A `TracedGlobal` taken out of its resource is traced by nothing from then on, so the next full
/// GC frees its traced node, while its value lives on and the handle still reaches it. Dropping
/// the `TracedGlobal` afterwards must leave the freed node alone: freeing it again would corrupt
/// V8's accounting of traced nodes, if not its free list.
#[test]
fn global_taken_out_of_a_traced_resource_outlives_its_traced_node() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let (resource, baseline) = holder_in_traced_mode(lock, ctx);
        let with_global = crate::Harness::used_global_handles_size(lock);

        let taken = resource.handle.take().expect("set above");
        // Nothing traces the node now; the value stays reachable through the global object.
        crate::Harness::request_gc(lock);
        let without_node = crate::Harness::used_global_handles_size(lock);
        assert!(without_node < with_global, "the GC frees the traced node");
        assert!(taken.as_local(lock).is_object());

        drop(taken);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        crate::Harness::request_gc(lock);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// A `TracedGlobal` put back into its resource after a GC freed its traced node must not hand that
/// node to the next trace. The visit installs a new traced node, which keeps the value alive
/// once nothing else does.
#[test]
fn global_put_back_after_its_traced_node_was_freed_is_traced_again() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let (resource, baseline) = holder_in_traced_mode(lock, ctx);
        let with_global = crate::Harness::used_global_handles_size(lock);

        let taken = resource.handle.take().expect("set above");
        crate::Harness::request_gc(lock);
        resource.handle.set(Some(taken));
        crate::Harness::request_gc(lock);
        assert_eq!(
            crate::Harness::used_global_handles_size(lock),
            with_global,
            "the visit installs one traced node in place of the freed one"
        );

        // The resource's trace alone keeps the value alive now.
        ctx.eval_raw(lock, "globalThis.value = undefined")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        let taken = resource.handle.take().expect("set above");
        assert!(taken.as_local(lock).is_object());

        drop(taken);
        crate::Harness::request_gc(lock);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// A resource without a wrapper that gains its first strong reference has its handles made
/// strong, by a visit outside any GC. A `TracedGlobal` put back into it after a GC freed its traced
/// node still has a weak strong handle, which that visit must make strong again along with
/// letting go of the node, or the value dies once nothing else holds it.
#[test]
fn global_put_back_into_a_resource_that_gains_a_strong_reference_is_strong() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        let parent = jsg::Rc::new(HolderParent { child });
        ctx.set_global(lock, "parent", parent.clone());
        ctx.eval_raw(
            lock,
            "parent.setChildValue(globalThis.value = {}); undefined",
        )
        .expect("should evaluate");
        // The parent's trace reaches the child, which has no strong reference: its `TracedGlobal`
        // goes to traced mode.
        crate::Harness::request_gc(lock);

        let taken = parent.child.handle.take().expect("set above");
        // Nothing traces the node now, and the GC frees it.
        crate::Harness::request_gc(lock);
        parent.child.handle.set(Some(taken));
        // The child's first strong reference visits the `TracedGlobal`.
        let child = parent.child.clone();

        // The `TracedGlobal` alone keeps the value alive now.
        ctx.eval_raw(lock, "globalThis.value = undefined; undefined")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        let taken = child.handle.take().expect("set above");
        assert!(taken.as_local(lock).is_object());
        Ok(())
    });
}

/// A `TracedGlobal` dropped while the isolate is not locked cannot reset its handle; the handle is
/// queued, keeps its value alive, and is reset the next time the isolate is locked.
#[test]
fn unlocked_drop_is_deferred_until_the_next_lock() {
    COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut wrapper: Option<v8::TracedGlobal<v8::Value>> = None;
    let mut baseline = 0;
    harness.run_in_context(|lock, _ctx| {
        // Created ahead of the measurements below, so that they only see the wrapper's handle.
        jsg::resource::function_template_of::<Counted>(lock);
        baseline = crate::Harness::used_global_handles_size(lock);

        let resource = jsg::Rc::new(Counted);
        wrapper = Some(resource.clone().to_js(lock).unwrap().to_traced_global(lock));
        // Only the `TracedGlobal` keeps the wrapper, and with it the resource, alive now.
        drop(resource);
        Ok(())
    });
    let wrapper = wrapper.expect("created in the context");

    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(
            COUNTED_DROPS.load(Ordering::SeqCst),
            0,
            "the TracedGlobal roots the wrapper"
        );
        let with_wrapper = crate::Harness::used_global_handles_size(lock);
        assert!(with_wrapper > baseline);
        assert_eq!(lock.globals_awaiting_lock(), 0);

        crate::Harness::with_isolate_unlocked(lock, || drop(wrapper));
        assert_eq!(lock.globals_awaiting_lock(), 1);

        crate::Harness::request_gc(lock);
        assert_eq!(
            COUNTED_DROPS.load(Ordering::SeqCst),
            0,
            "the queued handle still roots the wrapper"
        );
        assert_eq!(crate::Harness::used_global_handles_size(lock), with_wrapper);
        Ok(())
    });

    // Locking the isolate again resets the queued handle.
    harness.run_in_context(|lock, _ctx| {
        assert_eq!(lock.globals_awaiting_lock(), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(COUNTED_DROPS.load(Ordering::SeqCst), 1);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// The size of the handles in use when the isolate is next locked, after the lock has reset
/// the handles that were queued for it, less what that lock's own context adds: each
/// `run_in_context` call leaves a handle of its context in use until the next full GC, which
/// this measures from the call after. Like the count itself, that encodes V8's handle
/// accounting; see `Harness::used_global_handles_size`.
fn used_global_handles_size_at_next_lock(harness: &crate::Harness) -> usize {
    let mut at_next_lock = 0;
    harness.run_in_context(|lock, _ctx| {
        assert_eq!(lock.globals_awaiting_lock(), 0);
        at_next_lock = crate::Harness::used_global_handles_size(lock);
        Ok(())
    });
    let mut at_lock_after = 0;
    harness.run_in_context(|lock, _ctx| {
        at_lock_after = crate::Harness::used_global_handles_size(lock);
        Ok(())
    });
    at_next_lock - (at_lock_after - at_next_lock)
}

/// A `TracedGlobal` in traced mode dropped without the isolate lock queues its strong handle and
/// lets go of its traced node, which a full GC frees while the handle waits. The next lock resets the
/// handle and leaves the freed node alone.
#[test]
fn traced_global_dropped_without_the_lock_is_released_at_the_next_lock() {
    let harness = crate::Harness::new();
    let mut baseline = 0;
    let mut holder = None;
    harness.run_in_context(|lock, ctx| {
        let (resource, b) = holder_in_traced_mode(lock, ctx);
        baseline = b;
        let with_global = crate::Harness::used_global_handles_size(lock);
        let taken = resource.handle.take().expect("set above");
        crate::Harness::with_isolate_unlocked(lock, || drop(taken));
        assert_eq!(lock.globals_awaiting_lock(), 1);
        crate::Harness::request_gc(lock);
        assert!(
            crate::Harness::used_global_handles_size(lock) < with_global,
            "the GC frees the traced node"
        );
        // Kept, so that the release of its own handles does not change the count.
        holder = Some(resource);
        Ok(())
    });
    assert_eq!(used_global_handles_size_at_next_lock(&harness), baseline);
    drop(holder);
}

/// Wraps a new `HandleList` that collects a value, then GCs, which makes the first `TracedGlobal`'s
/// strong handle weak, then has the list grow and leaves it unreachable.
fn fill_and_abandon_handle_list(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let list = jsg::Rc::new(HandleList {
        handles: RefCell::new(Vec::new()),
    });
    ctx.set_global(lock, "list", list);
    ctx.eval_raw(lock, "list.push({})")
        .expect("should evaluate");
    crate::Harness::request_gc(lock);
    // Growing the `Vec` moves the first `TracedGlobal`, and frees the buffer it was in.
    ctx.eval_raw(lock, "for (let i = 0; i < 64; i++) list.push({})")
        .expect("should evaluate");
    ctx.eval_raw(lock, "globalThis.list = undefined")
        .expect("should evaluate");
}

/// Once GC has visited a `TracedGlobal`, V8 holds the address of its weak handle's slot, to null it
/// when the value dies. A `Vec` that grows moves its `TracedGlobal`s, so the slot must not move
/// with them. The values must die in the first GC after the move: a GC that visited them would keep
/// them alive.
#[test]
fn visited_globals_survive_being_moved() {
    HANDLE_LIST_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    // A first run leaves behind what V8 and the realm keep for the scripts and the type,
    // ahead of the measurement, so that the measurement only sees the second run's handles.
    harness.run_in_context(|lock, ctx| {
        fill_and_abandon_handle_list(lock, ctx);
        Ok(())
    });
    let mut baseline = 0;
    harness.run_in_context(|lock, ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(HANDLE_LIST_DROPS.load(Ordering::SeqCst), 1);
        baseline = crate::Harness::used_global_handles_size(lock);
        fill_and_abandon_handle_list(lock, ctx);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        // The first value dies with the list, and V8 nulls its weak slot; the list's drop
        // then resets every handle.
        crate::Harness::request_gc(lock);
        assert_eq!(HANDLE_LIST_DROPS.load(Ordering::SeqCst), 2);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// A `TracedGlobal` taken out of its resource keeps its weak handle, and V8 nulls that handle's
/// slot when the value dies, which it does at the next GC when nothing else holds it. The slot must
/// still be the one V8 knows, although the `TracedGlobal` has moved, or the `TracedGlobal` would go
/// on to reset a node V8 already freed.
#[test]
fn visited_global_taken_out_of_its_resource_sees_its_value_die() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        ctx.set_global(lock, "holder", resource.clone());
        crate::Harness::request_gc(lock);
        let baseline = crate::Harness::used_global_handles_size(lock);
        // The value is reachable through the resource's `TracedGlobal` alone.
        ctx.eval_raw(lock, "holder.set({})")
            .expect("should evaluate");
        // Tracing the wrapper puts the handle in traced mode: weak, with a traced node that
        // the resource's trace keeps alive.
        crate::Harness::request_gc(lock);
        let taken = resource.handle.take().expect("set above");
        // Nothing traces the node now, so the value dies and V8 nulls the weak handle.
        crate::Harness::request_gc(lock);
        drop(taken);
        assert_eq!(crate::Harness::used_global_handles_size(lock), baseline);
        Ok(())
    });
}

/// GC can still run once the `Realm` is dropped, and V8 then still nulls the weak handle slot of
/// a visited `TracedGlobal` when its value dies. The slot of a `TracedGlobal` dropped after the
/// `Realm` must therefore outlive that GC, or V8 writes into freed memory, which a memory checker
/// reports.
#[test]
fn visited_global_dropped_after_its_realm_keeps_its_slot_through_a_gc() {
    let harness = crate::Harness::new();
    let mut taken = None;
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        ctx.set_global(lock, "holder", resource.clone());
        ctx.eval_raw(lock, "holder.set({}); undefined")
            .expect("should evaluate");
        // Tracing the wrapper makes the handle weak.
        crate::Harness::request_gc(lock);
        taken = resource.handle.take();
        Ok(())
    });
    let taken = taken.expect("set in the context");
    // The weak handle is all that refers to the value, which dies in the GC.
    harness.drop_realm_then_gc(|| drop(taken));
}

/// A `TracedGlobal` is `Send`: another thread may drop it while this one holds the isolate lock and
/// creates handles. The drop is queued, and the next lock resets it. One of the `TracedGlobal`s is
/// in traced mode, so GC may write into its slot while it waits in the queue.
#[test]
fn drop_on_another_thread_is_deferred_until_the_next_lock() {
    COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    let mut wrapper: Option<v8::TracedGlobal<v8::Value>> = None;
    let mut visited: Option<v8::TracedGlobal<v8::Value>> = None;
    let mut holder = None;
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(Counted);
        wrapper = Some(resource.clone().to_js(lock).unwrap().to_traced_global(lock));
        drop(resource);

        let (resource, _) = holder_in_traced_mode(lock, ctx);
        visited = resource.handle.take();
        holder = Some(resource);
        Ok(())
    });
    let wrapper = wrapper.expect("created in the context");
    let visited = visited.expect("set in the context");

    harness.run_in_context(|lock, _ctx| {
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
        let dropper = std::thread::spawn({
            let barrier = std::sync::Arc::clone(&barrier);
            move || {
                barrier.wait();
                drop(wrapper);
                drop(visited);
            }
        });
        barrier.wait();
        // Meanwhile this thread, which holds the lock, keeps creating and dropping handles.
        for _ in 0..1000 {
            drop(lock.new_object().to_traced_global(lock));
        }
        dropper.join().expect("the drops do not panic");
        assert_eq!(lock.globals_awaiting_lock(), 2);
        crate::Harness::request_gc(lock);
        assert_eq!(COUNTED_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        assert_eq!(lock.globals_awaiting_lock(), 0);
        crate::Harness::request_gc(lock);
        assert_eq!(COUNTED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
    drop(holder);
}

/// `jsg::Rc::new` takes no `Lock`, so a thread without the isolate lock can put a `TracedGlobal`
/// into a new resource, whose first strong reference visits the `TracedGlobal`. That visit must
/// leave the handles alone while this thread holds the lock and creates handles: the strong handle
/// of a `TracedGlobal` in traced mode stays weak, so its value dies at the next GC once nothing
/// else holds it.
#[test]
fn visit_on_another_thread_leaves_a_global_alone() {
    COUNTED_DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(HandleHolder {
            handle: Cell::new(None),
        });
        ctx.set_global(lock, "holder", resource.clone());
        ctx.eval_raw(
            lock,
            "holder.set(globalThis.value = holder.newCounted()); undefined",
        )
        .expect("should evaluate");
        // Tracing the wrapper puts the `TracedGlobal` in traced mode.
        crate::Harness::request_gc(lock);
        let taken = resource.handle.take().expect("set above");

        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));
        let mover = std::thread::spawn({
            let barrier = std::sync::Arc::clone(&barrier);
            move || {
                barrier.wait();
                let holder = jsg::Rc::new(HandleHolder {
                    handle: Cell::new(Some(taken)),
                });
                holder.handle.take().expect("set above")
            }
        });
        barrier.wait();
        // Meanwhile this thread, which holds the lock, keeps creating and dropping handles.
        for _ in 0..1000 {
            drop(lock.new_object().to_traced_global(lock));
        }
        let taken = mover.join().expect("the move does not panic");

        ctx.eval_raw(lock, "globalThis.value = undefined; undefined")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(
            COUNTED_DROPS.load(Ordering::SeqCst),
            1,
            "the strong handle is still weak"
        );
        drop(taken);
        Ok(())
    });
}
