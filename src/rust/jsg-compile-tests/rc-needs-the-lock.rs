// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource's state and the reference counts of its `jsg::Rc`s and `jsg::Weak`s are guarded by
// the isolate lock, so every operation on them but dropping takes a `Lock`. Code that has none,
// such as a future that `IoContext` polls, can only drop them.
use std::cell::Cell;

use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: Cell<u32>,
}

#[jsg_resource]
impl Counter {}

fn require_clone<T: Clone>() {}

pub fn deref(counter: &jsg::Rc<Counter>) -> u32 {
    let _: &Counter = &**counter; //~ ERROR E0614
    counter.count.get() //~ ERROR E0609
}

pub fn clone(counter: &jsg::Rc<Counter>, weak: &jsg::Weak<Counter>) {
    let _ = counter.clone(); //~ ERROR E0061
    let _ = weak.clone(); //~ ERROR E0061
    require_clone::<jsg::Rc<Counter>>(); //~ ERROR E0277
    require_clone::<jsg::Weak<Counter>>(); //~ ERROR E0277
}

pub fn weak(counter: &jsg::Rc<Counter>, weak: &jsg::Weak<Counter>) {
    let _ = counter.downgrade(); //~ ERROR E0061
    let _ = weak.upgrade(); //~ ERROR E0061
    let _ = weak.is_alive(); //~ ERROR E0061
}

// Dropping needs no lock.
pub fn drop_them(counter: jsg::Rc<Counter>, weak: jsg::Weak<Counter>) {
    drop(counter);
    drop(weak);
}
