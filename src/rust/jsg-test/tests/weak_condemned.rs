// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `jsg::Weak` upgraded while the garbage collector sweeps the resource it points to.

use std::cell::Cell;
use std::sync::Mutex;

use jsg::ToJS;
use jsg_macros::jsg_resource;

thread_local! {
    /// The isolate whose collection the test runs, set only meanwhile, so that only the sweep's
    /// drops upgrade.
    static SWEEP_ISOLATE: Cell<*mut jsg::v8::ffi::Isolate> = const { Cell::new(std::ptr::null_mut()) };
    /// The `Peer`s dropped during that collection.
    static SWEPT: Cell<usize> = const { Cell::new(0) };
}
static REVIVED: Mutex<Vec<jsg::Rc<Peer>>> = Mutex::new(Vec::new());

/// One of a pair of resources that point at each other weakly. Its `Drop` upgrades the weak
/// reference, as code a C++ destructor calls into during the sweep can.
#[jsg_resource]
#[derive(Default)]
struct Peer {
    other: Cell<Option<jsg::Weak<Self>>>,
}

impl Drop for Peer {
    #[expect(unsafe_code, reason = "opens a scope for the GC that the test runs")]
    fn drop(&mut self) {
        let isolate = SWEEP_ISOLATE.get();
        if isolate.is_null() {
            return;
        }
        SWEPT.set(SWEPT.get() + 1);
        let Some(other) = self.other.take() else {
            return;
        };
        // SAFETY: the test's GC runs under the lock of `isolate` with a context entered, from
        // `request_gc`, whose `HandleScope` stays innermost while `scope` lives; the lock that
        // started the GC is not used meanwhile.
        let scope = std::pin::pin!(unsafe { jsg::CallbackScope::new(isolate) });
        jsg::catch_panic(scope, |lock| {
            let alive = other.is_alive(lock);
            let upgraded = other.upgrade(lock);
            assert_eq!(alive, upgraded.is_some(), "is_alive disagrees with upgrade");
            if let Some(rc) = upgraded {
                REVIVED.lock().expect("lock").push(rc);
            }
        });
    }
}

#[jsg_resource]
impl Peer {}

/// The collection that sweeps both resources of a pair sweeps one first, while the other is
/// condemned: its wrapper is dead, but its `Wrappable` still holds it. Upgrading a weak reference
/// to it then would revive a resource whose wrapper and traced handles the collection has
/// already reset.
#[test]
fn weak_upgrade_during_sweep_refuses_a_condemned_resource() {
    const PAIRS: usize = 8;

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        for _ in 0..PAIRS {
            let a = jsg::Rc::new(lock, Peer::default());
            let b = jsg::Rc::new(lock, Peer::default());
            a.get(lock).other.set(Some(b.downgrade(lock)));
            b.get(lock).other.set(Some(a.downgrade(lock)));
            // Each resource is held only by its wrapper, which nothing references.
            let _ = a.to_js(lock)?;
            let _ = b.to_js(lock)?;
        }
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        SWEPT.set(0);
        SWEEP_ISOLATE.set(lock.isolate().as_ffi());
        crate::Harness::request_gc(lock);
        SWEEP_ISOLATE.set(std::ptr::null_mut());
        Ok(())
    });
    let mut upgrades = 0;
    harness.run_in_context(|_lock, _ctx| {
        upgrades = std::mem::take(&mut *REVIVED.lock().expect("lock")).len();
        Ok(())
    });
    assert_eq!(upgrades, 0, "a weak reference revived a condemned resource");
    assert_eq!(
        SWEPT.get(),
        2 * PAIRS,
        "the collection sweeps both resources of every pair"
    );
}

/// A collection condemns only what it finds unreachable: a `jsg::Weak` to a resource that a live
/// wrapper holds still upgrades after it.
#[test]
fn weak_upgrade_after_gc_accepts_a_live_resource() {
    let harness = crate::Harness::new();
    let mut wrapper: Option<jsg::v8::Global<jsg::v8::Value>> = None;
    let mut weak: Option<jsg::Weak<Peer>> = None;
    harness.run_in_context(|lock, _ctx| {
        let peer = jsg::Rc::new(lock, Peer::default());
        weak = Some(peer.downgrade(lock));
        wrapper = Some(peer.to_js(lock)?.to_global(lock));
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        let weak = weak.take().expect("weak");
        assert!(weak.is_alive(lock));
        assert!(weak.upgrade(lock).is_some());
        drop(wrapper.take());
        Ok(())
    });
}
