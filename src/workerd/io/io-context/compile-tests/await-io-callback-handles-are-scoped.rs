// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The callback of `await_io` runs with a new lock, after the caller has returned: it cannot
// capture a handle of the caller's lock, as a handle is not `Send` and the callback is, and the
// handles it creates cannot outlive it.
use std::cell::RefCell;

use io_context::IoContext;
use jsg::v8;

thread_local! {
    static STASH: RefCell<Option<v8::Local<'static, v8::Object>>> = const { RefCell::new(None) };
}

pub fn captured<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let object: v8::Local<'s, v8::Value> = lock.new_object().into();
    context.await_io(lock, async {}, move |_lock, ()| Ok(object)) //~ ERROR cannot be sent between threads safely
}

pub fn stashed<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    context.await_io(lock, async {}, |lock, ()| {
        let object = lock.new_object();
        STASH.with(|stash| *stash.borrow_mut() = Some(object)); //~ ERROR E0521
        Ok(object.into())
    })
}
