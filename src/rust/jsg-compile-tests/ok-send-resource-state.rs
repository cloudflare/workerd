// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `jsg::Rc` or `jsg::Weak` of a resource whose state is `Send` is `Send` too: every use but
// dropping gates on a `Lock`, so on another thread it can only be dropped.
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource]
struct Holder {
    leaves: jsg::RefCell<Vec<jsg::Rc<Leaf>>>,
}

#[jsg_resource]
impl Holder {}

pub fn drop_elsewhere(holder: jsg::Rc<Holder>, weak: jsg::Weak<Leaf>) {
    std::thread::spawn(move || {
        drop(holder);
        drop(weak);
    });
}
