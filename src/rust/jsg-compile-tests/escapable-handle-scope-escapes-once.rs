// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// V8 lets a `v8::EscapableHandleScope` escape one handle, so `EscapableScope::escape` consumes
// the scope's slot. The slot can't leave the closure either, where its scope would be closed.
use jsg::v8;

pub fn escaped_twice<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    lock.with_escapable_handle_scope(|lock, scope| {
        let first = scope.escape(lock.new_object());
        let _ = scope.escape(lock.new_object()); //~ ERROR E0382
        first
    })
}

pub fn slot_kept<'s>(lock: &mut jsg::Lock<'s>) {
    let mut kept = None;
    lock.with_escapable_handle_scope(|_, scope| kept = Some(scope)); //~ ERROR E0521
    drop(kept);
}
