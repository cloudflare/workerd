// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg_macros::jsg_resource;

static PANIC_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct PanicsOnDrop {}

impl Drop for PanicsOnDrop {
    fn drop(&mut self) {
        PANIC_DROPS.fetch_add(1, Ordering::SeqCst);
        panic!("resource Drop panicked");
    }
}

#[jsg_resource]
impl PanicsOnDrop {}

/// A panic in a resource's `Drop` while the garbage collector sweeps it is logged and marks the
/// isolate to be aborted, and the process keeps running.
#[test]
fn panic_in_drop_during_gc_sweep() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let resource = jsg::Rc::new(lock, PanicsOnDrop {});
        let _wrapper = resource.to_js(lock)?;
        Ok(())
    });
    assert_eq!(PANIC_DROPS.load(Ordering::SeqCst), 0);
    harness.run_in_context(|lock, _ctx| {
        assert!(!crate::Harness::resource_drop_panicked(lock));
        crate::Harness::request_gc(lock);
        assert!(crate::Harness::resource_drop_panicked(lock));
        Ok(())
    });
    assert_eq!(PANIC_DROPS.load(Ordering::SeqCst), 1);
}
