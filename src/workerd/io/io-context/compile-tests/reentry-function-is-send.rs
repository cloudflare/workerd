// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The function of a reentry callback, the futures it returns and their output wait on the
// IoContext's event loop, which may move between OS threads, so none of them can hold a value that
// is not `Send`, such as an `std::rc::Rc`.
use std::rc::Rc;

use io_context::IoContext;
use io_context::ReentryCallback;

pub fn function(context: &IoContext<'_>, shared: Rc<u32>) -> jsg::Result<ReentryCallback<u32>> {
    context.make_reentry_callback_weak(move |_lock, _context| {
        //~^ ERROR cannot be sent between threads safely
        let value = *shared;
        Ok(async move { Ok(value) })
    })
}

pub fn future(context: &IoContext<'_>) -> jsg::Result<ReentryCallback<u32>> {
    context.make_reentry_callback_weak(|_lock, _context| {
        //~^ ERROR cannot be sent between threads safely
        let shared = Rc::new(1_u32);
        Ok(async move { Ok(*shared) })
    })
}

pub fn output(context: &IoContext<'_>) -> jsg::Result<ReentryCallback<Rc<u32>>> {
    context.make_reentry_callback_weak(|_lock, _context| Ok(async { Ok(Rc::new(1_u32)) }))
    //~^ ERROR cannot be sent between threads safely
}
