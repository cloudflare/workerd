// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The callback of `await_io` runs with a new lock, after the caller has returned: it cannot
// capture a handle of the caller's lock.
use io_context::IoContext;
use jsg::v8;

pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let object: v8::Local<'s, v8::Value> = lock.new_object().into();
    context.await_io(lock, async {}, move |_lock, ()| Ok(object)) //~ ERROR lifetime may not live long enough
}
