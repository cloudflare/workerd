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

fn decode_str(decoder: &mut Decoder, input: &[u8], flush: bool) -> Option<String> {
    let result = decode(decoder, input, &ffi::DecodeOptions { flush, fatal: true });
    if result.had_error {
        None
    } else {
        Some(String::from_utf16(result.output).unwrap())
    }
}

#[test]
fn iso_2022_jp_keeps_state_after_fatal_streaming_error() {
    let mut d = new_decoder(ffi::Encoding::Iso2022Jp).unwrap();
    // Switch to Roman, then hit an error mid-stream.
    assert_eq!(decode_str(&mut d, &[0x1b, 0x28, 0x4a, 0xff], false), None);
    assert_eq!(
        decode_str(&mut d, &[0x7e], true).as_deref(),
        Some("\u{203e}")
    );
    // The flush resets the decoder back to ASCII.
    assert_eq!(decode_str(&mut d, &[0x7e], true).as_deref(), Some("~"));
}

#[test]
fn fatal_error_on_flush_resets_decoder() {
    let mut d = new_decoder(ffi::Encoding::Iso2022Jp).unwrap();
    assert_eq!(decode_str(&mut d, &[0x1b, 0x28, 0x4a, 0xff], true), None);
    assert_eq!(decode_str(&mut d, &[0x7e], true).as_deref(), Some("~"));
}

#[test]
fn gb18030_keeps_restored_bytes_after_fatal_streaming_error() {
    let mut d = new_decoder(ffi::Encoding::Gb18030).unwrap();
    // 0x30 and 0x21 go back to the queue; 0x21 is left unread.
    assert_eq!(decode_str(&mut d, &[0x81, 0x30, 0x21, 0x21], false), None);
    assert_eq!(
        decode_str(&mut d, &[0x80], true).as_deref(),
        Some("0!!\u{20ac}")
    );
}

#[test]
fn euc_jp_decodes_unread_byte_after_fatal_streaming_error() {
    let mut d = new_decoder(ffi::Encoding::EucJp).unwrap();
    assert_eq!(decode_str(&mut d, &[0x8f, 0xa2, 0x20], false), None);
    assert_eq!(
        decode_str(&mut d, &[0xa1, 0xa1], true).as_deref(),
        Some(" \u{3000}")
    );
}

#[test]
fn empty_streaming_chunk_decodes_carried_bytes() {
    let mut d = new_decoder(ffi::Encoding::EucJp).unwrap();
    assert_eq!(decode_str(&mut d, &[0x8f, 0xa2, 0x20, 0x41], false), None);
    assert_eq!(decode_str(&mut d, &[], false).as_deref(), Some(" A"));
    assert_eq!(decode_str(&mut d, &[], true).as_deref(), Some(""));
}

#[test]
fn reset_drops_carried_bytes() {
    let mut d = new_decoder(ffi::Encoding::EucJp).unwrap();
    assert_eq!(decode_str(&mut d, &[0x8f, 0xa2, 0x20], false), None);
    reset(&mut d);
    assert_eq!(decode_str(&mut d, &[0x41], true).as_deref(), Some("A"));
}

#[test]
fn fatal_flush_error_drops_unread_bytes() {
    let mut d = new_decoder(ffi::Encoding::EucJp).unwrap();
    assert_eq!(decode_str(&mut d, &[0x8f, 0xa2, 0x20], true), None);
    assert_eq!(decode_str(&mut d, &[0x41], true).as_deref(), Some("A"));
}
