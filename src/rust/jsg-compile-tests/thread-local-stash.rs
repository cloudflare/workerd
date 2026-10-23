// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A handle cannot be stored anywhere that outlives its scope, such as a thread-local.
use jsg::v8;
thread_local! {
    static STASH: std::cell::Cell<Option<v8::Local<'static, v8::Object>>> =
        const { std::cell::Cell::new(None) };
}

pub fn t(lock: &mut jsg::Lock<'_>) {
    let object = lock.new_object();
    STASH.with(|cell| cell.set(Some(object))); //~ ERROR E0521
}
