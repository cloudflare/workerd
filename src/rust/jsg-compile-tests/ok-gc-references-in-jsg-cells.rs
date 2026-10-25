// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Traced state that a resource moves out of a field lives in a `jsg::Cell` or `jsg::RefCell`,
// whose `take`, `replace` and `borrow_mut` take the isolate and make a reference strong before
// it leaves the cell.
// A `std::cell::Cell` still holds anything with no GC references in it, such as a `jsg::Weak`.
use std::cell::Cell;

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource]
struct Holder {
    pending: jsg::Cell<Option<jsg::Rc<Leaf>>>,
    queue: jsg::RefCell<Vec<jsg::Rc<Leaf>>>,
    callback: jsg::Cell<Option<v8::TracedGlobal<v8::Value>>>,
    handles: jsg::RefCell<Vec<v8::TracedGlobal<v8::Value>>>,
    nested: Cell<Option<jsg::Weak<Leaf>>>,
    runs: Cell<u32>,
}

#[jsg_resource]
impl Holder {
    #[jsg_method]
    fn run(&self, lock: &mut jsg::Lock<'_>) {
        let _pending: Option<jsg::Rc<Leaf>> = self.pending.take(lock);
        let _next: Option<jsg::Rc<Leaf>> = self.queue.borrow_mut(lock).pop();
        let _count = self.queue.borrow().len();
        let _callback = self.callback.replace(lock, None);
        let _handle = self.handles.borrow_mut(lock).pop();
        let _nested = self.nested.take();
        self.runs.set(self.runs.get() + 1);
    }
}
