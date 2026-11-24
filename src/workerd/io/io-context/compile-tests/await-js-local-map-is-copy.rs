// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The conversion of `await_js_local` is the data of a reaction, which GC can drop on any
// thread that locks the isolate, so it captures nothing that is not `Copy` and `Send`, though its
// output need not be either.
use std::cell::Cell;
use std::rc::Rc;

use io_context::IoContext;
use jsg::v8;

pub fn captures(
    lock: &mut jsg::Lock<'_>,
    context: &IoContext<'_>,
    promise: v8::Local<'_, v8::Promise>,
) {
    let name = String::from("captured");
    let _ = context.await_js_local(lock, promise, move |_lock, _context, _value| {
        //~^ ERROR E0277
        Ok(name.len())
    });
}

pub fn captures_not_send(
    lock: &mut jsg::Lock<'_>,
    context: &IoContext<'_>,
    promise: v8::Local<'_, v8::Promise>,
    counter: &'static Cell<u32>,
) {
    let _ = context.await_js_local(lock, promise, move |_lock, _context, _value| {
        //~^ ERROR E0277
        Ok(counter.get())
    });
}

pub fn output_need_not_be_send<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    promise: v8::Local<'s, v8::Promise>,
) -> jsg::Result<io_context::AwaitJsLocal<Rc<u32>>> {
    context.await_js_local(lock, promise, |_lock, _context, _value| Ok(Rc::new(1_u32)))
}
