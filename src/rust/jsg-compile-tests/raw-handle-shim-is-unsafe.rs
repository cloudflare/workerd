// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A shim dereferences the raw handle it is given, which any integer can be made into, so it is
// unsafe.
use jsg::v8;

pub fn t() -> bool {
    v8::ffi::local_array_buffer_is_shared(&v8::ffi::Local { ptr: 1 })
    //~^ ERROR E0133
}
