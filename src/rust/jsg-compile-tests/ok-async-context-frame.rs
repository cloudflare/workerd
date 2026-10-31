// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An `AsyncContextFrame` is `'static`, `Send` and `Sync`, so a future passed to async I/O can hold
// it, and a `TracedAsyncContextFrame` is `Send` and can be a resource field. `enter` opens no
// scope, so a handle created in the frame stays valid after it.
use std::cell::Cell;

use jsg::v8;
use jsg_macros::jsg_resource;

fn assert_static_send_and_sync<T: Send + Sync + 'static>() {}
fn assert_static_send<T: Send + 'static>() {}

#[jsg_resource]
struct Holder {
    frame: Cell<Option<jsg::TracedAsyncContextFrame>>,
}

#[jsg_resource]
impl Holder {}

pub fn t<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    assert_static_send_and_sync::<jsg::AsyncContextFrame>();
    assert_static_send::<jsg::TracedAsyncContextFrame>();
    let frame = jsg::AsyncContextFrame::current(lock);
    let object = frame.enter(lock, |lock| lock.new_object());
    let holder = Holder {
        frame: Cell::new(Some(frame.into())),
    };
    drop(holder);
    object
}
