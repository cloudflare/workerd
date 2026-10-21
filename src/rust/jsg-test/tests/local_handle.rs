// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for how `Local` handles cross the FFI boundary.

use jsg::v8;

#[test]
fn from_ffi_rejects_an_empty_handle() {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: deliberately breaks the contract; `from_ffi` must panic before the handle
        // can be used.
        unsafe { v8::Local::<'static, v8::Value>::from_ffi(v8::ffi::Local { ptr: 0 }) };
    });
    assert!(result.is_err());
}
