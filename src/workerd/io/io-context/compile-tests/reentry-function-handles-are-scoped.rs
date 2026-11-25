// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The function of a reentry callback runs with a new lock each time it is called, after the caller
// has returned, and the future it returns is polled with none: neither can keep a handle, of the
// caller's lock or of its own. A `Local` is not `Send`, and the function and its future are, so
// neither can hold one.
use io_context::IoContext;
use jsg::v8;

pub fn captured<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>) {
    let object: v8::Local<'s, v8::Value> = lock.new_object().into();
    let _ = context.make_reentry_callback_weak(move |_lock, _context| {
        //~^ ERROR cannot be sent between threads safely
        let _object = object;
        Ok(async { Ok(()) })
    });
}

pub fn returned(context: &IoContext<'_>) {
    let _ = context.make_reentry_callback_weak(|lock, _context| {
        //~^ ERROR future cannot be sent between threads safely
        let object: v8::Local<'_, v8::Value> = lock.new_object().into();
        Ok(async move {
            let _object = object;
            Ok(())
        })
    });
}
