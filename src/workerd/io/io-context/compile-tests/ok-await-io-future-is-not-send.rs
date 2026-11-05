// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io` or `await_io_value` never leaves the IoContext's event loop, so
// it may hold values that are not `Send`: an `std::rc::Rc` of its own, a KJ promise, and a
// `jsg::Rc`, which it can only drop.
use std::cell::Cell;
use std::rc::Rc;

use io_context::IoContext;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: Cell<u32>,
}

#[jsg_resource]
impl Counter {}

pub fn wait<'s, P>(
    lock: &mut jsg::Lock<'s>,
    promise: P,
    data: P::Data,
    counter: jsg::Rc<Counter>,
) -> jsg::Result<v8::Local<'s, v8::Promise>>
where
    P: kj_rs::KjPromise + 'static,
    P::Data: 'static,
    P::Output: 'static,
{
    let context = IoContext::current(lock)?;
    context.await_io(
        lock,
        async move {
            let polls = Rc::new(Cell::new(0_u32));
            let shared = Rc::clone(&polls);
            let output = kj_rs::PromiseFuture::new(promise, data).await;
            shared.set(shared.get() + 1);
            drop(counter);
            (output.is_ok(), polls)
        },
        |lock, (ok, polls)| {
            let _ = ok;
            f64::from(polls.get()).to_js(lock)
        },
    )
}

pub fn wait_value<'s, P>(
    lock: &mut jsg::Lock<'s>,
    promise: P,
    data: P::Data,
) -> jsg::Result<v8::Local<'s, v8::Promise>>
where
    P: kj_rs::KjPromise + 'static,
    P::Data: 'static,
    P::Output: 'static,
{
    let context = IoContext::current(lock)?;
    context.await_io_value(lock, async move {
        let polls = Rc::new(Cell::new(0_u32));
        let _ = kj_rs::PromiseFuture::new(promise, data).await;
        polls.set(polls.get() + 1);
        Ok::<_, jsg::Error>(f64::from(polls.get()))
    })
}
