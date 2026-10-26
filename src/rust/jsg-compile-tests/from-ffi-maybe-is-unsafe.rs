// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Turning a C++ handle that may be empty into an `Option<Local>` picks its scope, so it is unsafe.
use jsg::v8;

pub fn t(handle: v8::ffi::MaybeLocal) {
    let _: Option<v8::Local<'_, v8::Value>> = v8::Local::from_ffi_maybe(handle);
    //~^ ERROR E0133
}
