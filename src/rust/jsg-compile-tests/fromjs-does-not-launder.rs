// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `FromJS` for a handle type returns a handle of the same scope as its input.
use jsg::FromJS;
use jsg::v8;

pub fn t(isolate: *mut v8::ffi::Isolate) {
    let keep: Option<v8::Local<'_, v8::Uint8Array>>;
    {
        // SAFETY: only type-checked.
        jsg::callback_scope!(unsafe let lock, isolate);
        //~^ ERROR E0716
        //~| ERROR E0716
        let value = lock.undefined();
        keep = <v8::Local<'_, v8::Uint8Array> as FromJS<'_>>::from_js(lock, value).ok();
    }
    let _ = keep;
}
