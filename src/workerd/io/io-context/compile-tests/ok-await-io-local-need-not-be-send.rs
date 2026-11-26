// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io_local` and its output stay on the IoContext's thread, so neither
// need be `Send`: each can hold an `Rc`, as the future of a C++ `kj::Promise` holds the promise.
// It is `unsafe`: the caller promises that the event loop stays on its thread.
use std::rc::Rc;

use io_context::IoContext;
use jsg::ToJS;

#[allow(unsafe_code, reason = "calls the IoContext's unsafe non-Send APIs")]
pub fn future<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>, shared: Rc<u32>) {
    // SAFETY: the snippet is only compiled, never run.
    let _ = unsafe {
        context.await_io_local(lock, async move { *shared }, |lock, value| {
            f64::from(value).to_js(lock)
        })
    };
}

#[allow(unsafe_code, reason = "calls the IoContext's unsafe non-Send APIs")]
pub fn output<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>) {
    // SAFETY: the snippet is only compiled, never run.
    let _ = unsafe {
        context.await_io_local(lock, async { Rc::new(1_u32) }, |lock, value| {
            f64::from(*value).to_js(lock)
        })
    };
}
