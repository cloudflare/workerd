// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The callback of `await_io_and_run` gets a new lock; the handles it creates cannot outlive the
// callback.
use std::cell::RefCell;

use io_context::IoContext;
use jsg::v8;

thread_local! {
    static STASH: RefCell<Option<v8::Local<'static, v8::Object>>> = const { RefCell::new(None) };
}

pub fn t(context: &IoContext<'_>) {
    let _ = context.await_io_and_run(async {}, |lock, ()| {
        STASH.with(|stash| *stash.borrow_mut() = Some(lock.new_object())); //~ ERROR E0521
        Ok(())
    });
}
