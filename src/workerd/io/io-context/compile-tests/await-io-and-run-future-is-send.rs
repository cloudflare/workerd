// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `await_io_and_run` takes only a future that is `Send`, so it cannot hold a value that is not,
// such as an `std::rc::Rc`. `await_io_local` takes one that is only `'static`.
use std::rc::Rc;

use io_context::IoContext;

pub fn t(context: &IoContext<'_>, shared: Rc<u32>) {
    let _ = context.await_io_and_run(async move { *shared }, |_lock, _value| Ok(())); //~ ERROR future cannot be sent between threads safely
}
