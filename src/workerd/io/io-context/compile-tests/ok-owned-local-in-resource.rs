// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource holds a value that belongs to the IoContext's thread, here an `Rc`, through the
// handle the IoContext gives for it, which is `Send` and `Traced`, as a resource's fields must be.
use std::cell::Cell;
use std::rc::Rc;

use io_context::IoContext;
use io_context::IoOwnedLocal;

#[jsg_macros::jsg_resource]
pub struct Counter {
    count: IoOwnedLocal<Rc<Cell<u32>>>,
}

#[jsg_macros::jsg_resource]
impl Counter {}

#[allow(unsafe_code, reason = "calls the IoContext's unsafe non-Send APIs")]
pub fn t(lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
    let context = IoContext::current(lock)?;
    let counter = Counter {
        // SAFETY: the snippet is only compiled, never run.
        count: unsafe { context.add_local_object(Rc::new(Cell::new(0))) }?,
    };
    counter
        .count
        .with(lock, |count| count.set(count.get() + 1))?;
    let _count: Option<Rc<Cell<u32>>> = counter.count.take(lock)?;
    Ok(())
}
