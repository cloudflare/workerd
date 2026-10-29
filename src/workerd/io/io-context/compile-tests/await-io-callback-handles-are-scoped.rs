// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The callback of `await_io` gets a new lock; the handles it creates cannot outlive the callback.
use std::cell::RefCell;
use std::rc::Rc;

use io_context::IoContext;
use jsg::v8;

pub fn t(context: &IoContext<'_>, stash: Rc<RefCell<Option<v8::Local<'static, v8::Object>>>>) {
    let _ = context.await_io(async {}, move |lock, ()| {
        *stash.borrow_mut() = Some(lock.new_object()); //~ ERROR E0521
        Ok(())
    });
}
