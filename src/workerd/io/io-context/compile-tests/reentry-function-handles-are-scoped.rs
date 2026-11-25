// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The function of a reentry callback runs with a new lock each time it is called, after the caller
// has returned, and the future it returns is polled with none: neither can keep a handle, of the
// caller's lock or of its own. The function is `'static`, so it can't capture a `Local` of the
// caller's lock, and its future is one type for every lock, so it can't hold a `Local` of the
// lock it is given.
use io_context::IoContext;
use jsg::v8;

pub fn captured<'s>(lock: &mut jsg::Lock<'s>, context: &IoContext<'s>) {
    let object: v8::Local<'s, v8::Value> = lock.new_object().into();
    let _ = context.make_reentry_callback_weak(move |_lock, _context| {
        //~^ ERROR E0521
        let _object = object;
        Ok(async { Ok(()) })
    });
}

pub fn returned(context: &IoContext<'_>) {
    let _ = context.make_reentry_callback_weak(|lock, _context| {
        let object: v8::Local<'_, v8::Value> = lock.new_object().into();
        Ok(async move {
            //~^ ERROR lifetime may not live long enough
            let _object = object;
            Ok(())
        })
    });
}
