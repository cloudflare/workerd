// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future of `await_js_with_context` may take its output on a later turn of the IoContext's
// event loop, which may by then run on another OS thread, so the output is `Send`.
use std::rc::Rc;

use io_context::AwaitJsWithContext;
use io_context::IoContext;
use jsg::v8;

pub fn output<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    promise: v8::Local<'s, v8::Promise>,
) -> jsg::Result<AwaitJsWithContext<Rc<u32>>> {
    context.await_js_with_context(lock, promise, |_lock, _context, _value| Ok(Rc::new(1_u32)))
    //~^ ERROR cannot be sent between threads safely
}
