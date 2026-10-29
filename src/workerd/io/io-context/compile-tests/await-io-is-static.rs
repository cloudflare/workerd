// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io`, and the callback that runs after it, run after the caller has
// returned, so neither can borrow from it.
use io_context::IoContext;

pub fn future(context: &IoContext<'_>, data: &[u8]) {
    let _ = context.await_io(async { data.len() }, |_lock, _len| Ok(())); //~ ERROR E0521
}

pub fn callback(context: &IoContext<'_>, data: &[u8]) {
    let _ = context.await_io(async {}, |_lock, ()| {
        //~^ ERROR E0521
        let _ = data.len();
        Ok(())
    });
}
