// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

/// Exercises the full V8 path: allocate, convert into the backing store,
/// narrow the view. `dispatch-test.rs` covers conversion behaviour itself; this
/// checks the parts that only exist once V8 is involved.
#[test]
fn transcodes_into_a_narrowed_uint8_array() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // '☕' is three UTF-8 bytes and transcodes to the single byte "?"
        // in ASCII, so the destination is allocated at 3 bytes and the
        // returned view must be narrowed to 1.
        let source = "☕".as_bytes();
        let array =
            transcode_impl(lock, source, ffi::Encoding::Utf8, ffi::Encoding::Ascii).unwrap();
        assert_eq!(array.len(), 1);
        assert_eq!(array.as_slice(), b"?");
        Ok(())
    });
}

#[test]
fn empty_input_yields_an_empty_uint8_array() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let array = transcode_impl(lock, &[], ffi::Encoding::Utf8, ffi::Encoding::Utf16Le).unwrap();
        assert!(array.is_empty());
        Ok(())
    });
}

#[test]
fn full_length_result_is_not_narrowed() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // Latin-1 -> UTF-16LE widens every byte to exactly two, so the
        // conversion fills the destination exactly.
        let array =
            transcode_impl(lock, b"Hi", ffi::Encoding::Latin1, ffi::Encoding::Utf16Le).unwrap();
        assert_eq!(array.as_slice(), &[0x48, 0x00, 0x69, 0x00]);
        Ok(())
    });
}

#[test]
fn failure_surfaces_as_an_error() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // Odd-length UTF-16LE input is rejected before any allocation.
        let result = transcode_impl(lock, &[0x61], ffi::Encoding::Utf16Le, ffi::Encoding::Utf8);
        assert!(result.is_err());
        Ok(())
    });
}
