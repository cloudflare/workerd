// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io_local`, and the callback that runs after it, run after the
// caller has returned, so neither can borrow from it. Being `'static` is what keeps a future that
// need not be `Send` from holding what is scoped to a lock.
use io_context::IoContext;
use jsg::ToJS;

pub fn future<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>, data: &[u8]) {
    let _ = context.await_io_local(lock, async { data.len() }, |lock, len| {
        //~^ ERROR E0521
        f64::from(u32::try_from(len).unwrap_or(0)).to_js(lock)
    });
}

pub fn callback<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>, data: &[u8]) {
    let _ = context.await_io_local(lock, async {}, |lock, ()| {
        //~^ ERROR E0521
        f64::from(u32::try_from(data.len()).unwrap_or(0)).to_js(lock)
    });
}
