// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The entry point trusts its caller about the isolate's state, so it is unsafe.
use jsg::v8;
pub fn storage(isolate: *mut v8::ffi::Isolate) {
    let _storage = jsg::CallbackScope::new(isolate);
    //~^ ERROR E0133
}
