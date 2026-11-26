// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A handle created in `with_handle_scope` dies when the scope closes, so it can be neither
// returned from the closure nor stored where the enclosing scope can reach it. The closure can't
// use the enclosing lock either, which would create handles of the enclosing scope's lifetime in
// the new scope.
use jsg::v8;

pub fn returned<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Object> {
    lock.with_handle_scope(|lock, _| lock.new_object()) //~ ERROR lifetime may not live long enough
}

pub fn stored<'s>(lock: &mut jsg::Lock<'s>) {
    let mut escaped: Option<v8::Local<'s, v8::Object>> = None;
    lock.with_handle_scope(|lock, _| escaped = Some(lock.new_object())); //~ ERROR E0521
    drop(escaped);
}

pub fn lock_kept<'s>(lock: &mut jsg::Lock<'s>) {
    let mut kept = None;
    lock.with_handle_scope(|lock, _| kept = Some(lock)); //~ ERROR E0521
    drop(kept);
}

pub fn outer_lock_used<'s>(lock: &mut jsg::Lock<'s>) {
    lock.with_handle_scope(|_, _| {
        //~^ ERROR E0500
        //~| ERROR E0501
        let _ = lock.new_object();
    });
}
