// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io` is polled without the isolate lock, so it cannot hold a value
// that is not `Send`, and its output, which waits for the lock, cannot be one either.
use std::rc::Rc;

use io_context::IoContext;
use jsg::ToJS;

pub fn future(lock: &mut jsg::Lock<'_>, context: &IoContext<'_>, shared: Rc<u32>) {
    let _ = context.await_io(lock, async move { *shared }, |lock, value| {
        //~^ ERROR future cannot be sent between threads safely
        f64::from(value).to_js(lock)
    });
}

pub fn output(lock: &mut jsg::Lock<'_>, context: &IoContext<'_>) {
    let _ = context.await_io(lock, async { Rc::new(1_u32) }, |lock, value| {
        //~^ ERROR E0277: `std::rc::Rc<u32>` cannot be sent between threads safely
        f64::from(*value).to_js(lock)
    });
}
