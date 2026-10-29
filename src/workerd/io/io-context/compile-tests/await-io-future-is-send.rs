// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io` is polled without the isolate lock, so it cannot hold a value
// that is not `Send`, such as a `jsg::Rc` (which holds a `std::rc::Rc`) or anything else bound to
// the isolate.
use std::rc::Rc;

use io_context::IoContext;

pub fn t(context: &IoContext<'_>, shared: Rc<u32>) {
    let _ = context.await_io(async move { *shared }, |_lock, _value| Ok(())); //~ ERROR future cannot be sent between threads safely
}
