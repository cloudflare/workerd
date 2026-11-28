// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The callback given to `await_io_and_run` waits on the event loop with the future, and an event
// loop may move between OS threads, so it cannot hold a value that is not `Send`, such as an
// `std::rc::Rc`.
use std::rc::Rc;

use io_context::IoContext;

pub fn t(context: &IoContext<'_>, shared: Rc<u32>) {
    let callback = move |_lock: &mut jsg::Lock<'_>, ()| {
        let _ = *shared;
        Ok(())
    };
    let _ = context.await_io_and_run(async {}, callback); //~ ERROR cannot be sent between threads safely
}
