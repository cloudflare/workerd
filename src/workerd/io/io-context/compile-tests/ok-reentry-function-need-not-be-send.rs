// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The function of a reentry callback, the future it returns and that future's output stay on the
// IoContext's thread, so none of them need be `Send`: each can hold an `Rc`, as an output can hold
// C++ objects that belong to that thread.
use std::rc::Rc;

use io_context::IoContext;
use io_context::ReentryCallback;

pub fn function(context: &IoContext<'_>, shared: Rc<u32>) -> jsg::Result<ReentryCallback<u32>> {
    context.make_reentry_callback_weak(move |_lock, _context| {
        let value = *shared;
        Ok(async move { Ok(value) })
    })
}

pub fn future(context: &IoContext<'_>) -> jsg::Result<ReentryCallback<u32>> {
    context.make_reentry_callback_weak(|_lock, _context| {
        let shared = Rc::new(1_u32);
        Ok(async move { Ok(*shared) })
    })
}

pub fn output(context: &IoContext<'_>) -> jsg::Result<ReentryCallback<Rc<u32>>> {
    context.make_reentry_callback_weak(|_lock, _context| Ok(async { Ok(Rc::new(1_u32)) }))
}

pub async fn call(callback: &ReentryCallback<Rc<u32>>) -> Option<u32> {
    Some(*callback.call().await.ok()?)
}
