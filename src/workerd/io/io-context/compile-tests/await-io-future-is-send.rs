// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `await_io` takes only a future and an output that are `Send`, so neither can be a value that is
// not. `await_io_local` takes ones that are only `'static`.
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
