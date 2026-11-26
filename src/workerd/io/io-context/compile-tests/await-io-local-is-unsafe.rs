// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What `await_io_local` and `add_local_object` take may be tied to the OS thread, so calling them
// is `unsafe`: only a caller that knows the event loop stays on its thread may.
use std::rc::Rc;

use io_context::IoContext;
use jsg::ToJS;

pub fn await_io<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>) {
    let _ = context.await_io_local(lock, async { 1_u32 }, |lock, value| {
        //~^ ERROR E0133
        f64::from(value).to_js(lock)
    });
}

pub fn add_object(context: &IoContext<'_>) {
    let _ = context.add_local_object(Rc::new(1_u32));
    //~^ ERROR E0133
}
