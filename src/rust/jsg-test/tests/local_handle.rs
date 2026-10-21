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

#[test]
fn empty_cpp_maybe_local_reaches_rust_as_none() {
    crate::Harness::new().run_in_context(|_lock, _ctx| {
        // SAFETY: the handle is empty.
        let local: Option<v8::Local<'_, v8::Value>> =
            unsafe { v8::Local::from_ffi_maybe(crate::ffi::empty_maybe_local()) };
        assert!(local.is_none());
        Ok(())
    });
}

#[test]
fn option_local_round_trips_through_cpp_maybe_local() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        // SAFETY: the handles are used only while `lock`'s scope is open.
        unsafe {
            assert!(crate::ffi::cpp_sees_empty_maybe_local(
                v8::Local::<v8::Value>::into_ffi_maybe(None)
            ));

            let value = lock.null();
            let maybe = v8::Local::into_ffi_maybe(Some(value));
            assert!(!crate::ffi::cpp_sees_empty_maybe_local(maybe));
            let back: Option<v8::Local<'_, v8::Value>> = v8::Local::from_ffi_maybe(maybe);
            assert_eq!(back, Some(value));
        }
        Ok(())
    });
}
