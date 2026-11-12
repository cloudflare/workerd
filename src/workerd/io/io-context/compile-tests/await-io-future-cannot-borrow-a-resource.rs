// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io_and_run` runs without the isolate lock, so it cannot keep a
// resource borrowed from a `jsg::Rc`: the borrow lasts no longer than the lock's scope, even when
// the `jsg::Rc` itself lives forever.
use io_context::IoContext;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: u32,
}

#[jsg_resource]
impl Counter {}

pub fn leaked<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    counter: &'static jsg::Rc<Counter>,
) {
    let counter: &Counter = counter.get(lock);
    let _ = context.await_io_and_run(async move { counter.count }, |_lock, _count| Ok(())); //~ ERROR E0521
}
