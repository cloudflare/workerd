// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn known_encoding_constructs_a_decoder() {
    assert!(new_decoder(ffi::Encoding::Windows1252).is_ok());
}

#[test]
fn unknown_encoding_returns_error_instead_of_panicking() {
    // Simulate discriminant drift / a corrupt value from C++: an out-of-range
    // `Encoding` (the shared cxx enum is a `#[repr(u16)]` open value). This must
    // surface as a clean error rather than aborting the process.
    let bad = ffi::Encoding { repr: u16::MAX };
    match new_decoder(bad) {
        Ok(_) => panic!("out-of-range discriminant should error"),
        Err(err) => assert!(err.description().contains("unknown encoding discriminant")),
    }
}
