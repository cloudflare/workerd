// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `TracedGlobal` outlives scopes and is reopened in a later one.
use jsg::v8;
pub fn t(isolate: *mut v8::ffi::Isolate) {
    let global = {
        // SAFETY: only type-checked.
        jsg::callback_scope!(unsafe let lock, isolate);
        let object = lock.new_object();
        object.to_traced_global(lock)
    };
    // SAFETY: only type-checked.
    jsg::callback_scope!(unsafe let lock, isolate);
    let object = global.as_local(lock);
    let _ = object.has(lock, "x");
}
