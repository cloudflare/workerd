// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Resource state may hold `jsg::Rc`s, which every use but dropping gates on a `Lock`, and data
// shared through an `Arc` of `Sync` data. Both are `Send`, so a `jsg::Rc` or `jsg::Weak` can
// move to another thread, where it can only be dropped.
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicU32;

use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource]
struct Holder {
    leaves: jsg::RefCell<Vec<jsg::Rc<Leaf>>>,
    shared: Arc<Mutex<u32>>,
    count: Arc<AtomicU32>,
}

#[jsg_resource]
impl Holder {}

pub fn drop_elsewhere(leaf: jsg::Rc<Leaf>, weak: jsg::Weak<Leaf>) {
    std::thread::spawn(move || {
        drop(leaf);
        drop(weak);
    });
}
