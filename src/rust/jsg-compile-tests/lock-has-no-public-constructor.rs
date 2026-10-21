// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// There is no way to make a lock from a raw isolate pointer other than an entry point such as
// `jsg::callback_scope!`.
use jsg::v8;
pub fn t(isolate: *mut v8::ffi::Isolate) {
    // SAFETY: only type-checked.
    let _lock = unsafe { jsg::Lock::from_isolate_ptr(isolate) }; //~ ERROR E0599
}
