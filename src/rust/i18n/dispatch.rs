// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The transcoding dispatch table and per-pair conversion logic, ported from
//! `i18n.c++`'s `TranscodeDefault` / `TranscodeLatin1ToUTF16` /
//! `TranscodeFromUTF16` / `TranscodeUTF16FromUTF8` / `TranscodeUTF8FromUTF16`
//! and the `switch` in `transcode()` that picks between them. All sizing,
//! substitute-character setup, empty-input handling, and truncation happens
//! here; [`crate::shim`] only forwards to the underlying ICU/simdutf calls.

use crate::error::TranscodeError;
use crate::ffi::Encoding;
use crate::shim;
use crate::shim::Converter;

/// An isolate has a 128MB memory limit. Mirrors `ISOLATE_LIMIT` in `i18n.c++`.
const ISOLATE_LIMIT: usize = 134_217_728;

/// Transcodes `source` from `from` to `to`, matching the dispatch table built
/// by `i18n::transcode()` in `i18n.c++`.
pub fn transcode(source: &[u8], from: Encoding, to: Encoding) -> Result<Vec<u8>, TranscodeError> {
    let result = match (from, to) {
        (Encoding::Ascii | Encoding::Latin1, Encoding::Utf16Le) => {
            transcode_latin1_to_utf16(source)?
        }
        (Encoding::Utf8, Encoding::Utf16Le) => transcode_utf16_from_utf8(source)?,
        (Encoding::Utf16Le, Encoding::Utf8) => transcode_utf8_from_utf16(source)?,
        (Encoding::Utf16Le, Encoding::Ascii | Encoding::Latin1) => {
            transcode_from_utf16(source, to)?
        }
        // TranscodeDefault: identity conversions, UTF16LE -> UTF16LE, and every
        // other (from, to) pair not overridden above -- mirrors the default
        // `TranscodeImpl transcode_function = &TranscodeDefault;` in
        // `i18n::transcode()`.
        _ => transcode_default(source, from, to)?,
    };
    result.ok_or(TranscodeError::UnableToTranscode)
}

/// Mirrors `TranscodeDefault`: a plain ICU `ucnv_convertEx` between two
/// converters. Used for every (from, to) pair not handled by simdutf, plus
/// the UTF16LE -> UTF16LE identity conversion.
fn transcode_default(
    source: &[u8],
    from: Encoding,
    to: Encoding,
) -> Result<Option<Vec<u8>>, TranscodeError> {
    let to_conv = Converter::open(to);
    let substitute = "?".repeat(to_conv.min_char_size());
    to_conv.set_subst_chars(&substitute);
    let from_conv = Converter::open(from);

    let limit = source
        .len()
        .checked_mul(to_conv.max_char_size())
        .ok_or(TranscodeError::SourceBufferTooLarge)?;
    if limit == 0 {
        return Ok(Some(Vec::new()));
    }
    // Workers are limited to 128MB so this isn't actually a realistic concern,
    // but sanity check.
    if limit > ISOLATE_LIMIT {
        return Err(TranscodeError::SourceBufferTooLarge);
    }

    let mut out = vec![0u8; limit];
    Ok(
        shim::convert_ex(&to_conv, &from_conv, source, &mut out).map(|written| {
            out.truncate(written);
            out
        }),
    )
}

/// Mirrors `TranscodeLatin1ToUTF16`: widens ASCII/LATIN1 `source` into UTF-16
/// via simdutf. Source bytes `0x80`-`0xFF` become U+0080-U+00FF rather than
/// being substituted -- this is the "latin1" path, taken even when `from` is
/// `ASCII` (see R8).
fn transcode_latin1_to_utf16(source: &[u8]) -> Result<Option<Vec<u8>>, TranscodeError> {
    let length_in_chars = source
        .len()
        .checked_mul(2)
        .ok_or(TranscodeError::SourceBufferTooLarge)?;
    // Workers are limited to 128MB so this isn't actually a realistic concern,
    // but sanity check.
    if length_in_chars > ISOLATE_LIMIT {
        return Err(TranscodeError::SourceBufferTooLarge);
    }
    if length_in_chars == 0 {
        return Ok(Some(Vec::new()));
    }

    let mut dest = vec![0u8; length_in_chars];
    let actual_length = shim::convert_latin1_to_utf16(source, &mut dest);
    // simdutf returns 0 for invalid input.
    if actual_length == 0 {
        return Ok(None);
    }
    dest.truncate(actual_length * 2);
    Ok(Some(dest))
}

/// Mirrors `TranscodeFromUTF16`: `ucnv_fromUChars` from UTF-16LE `source` into
/// `to`'s encoding (ASCII or LATIN1).
fn transcode_from_utf16(source: &[u8], to: Encoding) -> Result<Option<Vec<u8>>, TranscodeError> {
    let to_conv = Converter::open(to);
    let substitute = "?".repeat(to_conv.min_char_size());
    to_conv.set_subst_chars(&substitute);

    if !source.len().is_multiple_of(2) {
        return Err(TranscodeError::OddUtf16leInput);
    }
    let utf16_len = source.len() / 2;

    let limit = utf16_len
        .checked_mul(to_conv.max_char_size())
        .ok_or(TranscodeError::BufferTooLarge)?;
    // Workers are limited to 128MB so this isn't actually a realistic concern,
    // but sanity check.
    if limit > ISOLATE_LIMIT {
        return Err(TranscodeError::BufferTooLarge);
    }
    if limit == 0 {
        return Ok(Some(Vec::new()));
    }

    let mut dest = vec![0u8; limit];
    Ok(
        shim::from_uchars(&to_conv, source, &mut dest).map(|written| {
            dest.truncate(written);
            dest
        }),
    )
}

/// Mirrors `TranscodeUTF16FromUTF8`: converts UTF-8 `source` to UTF-16LE via
/// simdutf. The output size comes from `simdutf::utf16_length_from_utf8`; when
/// that estimate is zero the result is an empty buffer, even for non-empty
/// input (see R8).
fn transcode_utf16_from_utf8(source: &[u8]) -> Result<Option<Vec<u8>>, TranscodeError> {
    let expected_utf16_length = shim::utf16_length_from_utf8(source);
    // Workers are limited to 128MB so this isn't actually a realistic concern,
    // but sanity check.
    if expected_utf16_length > ISOLATE_LIMIT {
        return Err(TranscodeError::ExpectedUtf16LengthTooLarge);
    }

    let length_in_chars = expected_utf16_length
        .checked_mul(2)
        .ok_or(TranscodeError::ExpectedUtf16LengthTooLarge)?;
    if length_in_chars == 0 {
        return Ok(Some(Vec::new()));
    }

    let mut dest = vec![0u8; length_in_chars];
    let actual_length = shim::convert_utf8_to_utf16le(source, &mut dest);
    // simdutf returns 0 for invalid UTF-8 input.
    if actual_length == 0 {
        return Ok(None);
    }
    if actual_length != expected_utf16_length {
        return Err(TranscodeError::Utf16LengthMismatch);
    }
    Ok(Some(dest))
}

/// Mirrors `TranscodeUTF8FromUTF16`: converts UTF-16LE `source` to UTF-8 via
/// simdutf, requiring the actual conversion length to equal the estimate from
/// `simdutf::utf8_length_from_utf16le`.
fn transcode_utf8_from_utf16(source: &[u8]) -> Result<Option<Vec<u8>>, TranscodeError> {
    if !source.len().is_multiple_of(2) {
        return Err(TranscodeError::OddUtf16leInput);
    }

    let expected_utf8_length = shim::utf8_length_from_utf16le(source);
    // Workers are limited to 128MB so this isn't actually a realistic concern,
    // but sanity check.
    if expected_utf8_length > ISOLATE_LIMIT {
        return Err(TranscodeError::ExpectedUtf8LengthTooLarge);
    }
    if expected_utf8_length == 0 {
        return Ok(Some(Vec::new()));
    }

    let mut dest = vec![0u8; expected_utf8_length];
    let actual_length = shim::convert_utf16le_to_utf8(source, &mut dest);
    if actual_length != expected_utf8_length {
        return Err(TranscodeError::Utf8LengthMismatch);
    }
    // Unreachable in practice: `actual_length` was just required to equal
    // `expected_utf8_length`, which is nonzero at this point. This mirrors the
    // equivalent dead branch in the C++ `TranscodeUTF8FromUTF16`, which checks
    // `actual_length == 0` only *after* already requiring it to equal the
    // (nonzero) expected length above -- so a simdutf failure (return value 0)
    // surfaces as "Expected UTF8 length mismatch" here, not
    // "Unable to transcode buffer", unlike every other conversion direction.
    if actual_length == 0 {
        return Ok(None);
    }
    Ok(Some(dest))
}

#[cfg(test)]
mod tests {
    use jsg_test::Harness;

    use super::*;

    /// All four transcodable encodings, for exhaustively testing every pair.
    const ENCODINGS: [Encoding; 4] = [
        Encoding::Ascii,
        Encoding::Latin1,
        Encoding::Utf8,
        Encoding::Utf16Le,
    ];

    // `Harness::new()` initializes the V8 platform, which is what installs the
    // embedded ICU data. ICU converter opens fail without it, even though
    // these tests never create an isolate or run JavaScript.
    fn init_icu() -> Harness {
        Harness::new()
    }

    #[test]
    fn every_pair_round_trips_ascii_text() {
        let _harness = init_icu();
        for &from in &ENCODINGS {
            // UTF16LE source bytes must have even length; every other encoding
            // is happy with plain ASCII bytes.
            let source: &[u8] = if from == Encoding::Utf16Le {
                &[0x48, 0x00, 0x69, 0x00] // "Hi" as UTF-16LE code units.
            } else {
                b"Hi"
            };
            for &to in &ENCODINGS {
                let result = transcode(source, from, to);
                assert!(result.is_ok(), "{from:?} -> {to:?} failed: {result:?}");
            }
        }
    }

    #[test]
    fn every_pair_empty_input_is_empty_output() {
        let _harness = init_icu();
        for &from in &ENCODINGS {
            for &to in &ENCODINGS {
                let result = transcode(&[], from, to);
                assert_eq!(
                    result.as_deref(),
                    Ok([].as_slice()),
                    "{from:?} -> {to:?} on empty input should be empty, got {result:?}"
                );
            }
        }
    }

    #[test]
    fn identity_pairs_round_trip() {
        let _harness = init_icu();
        for &encoding in &ENCODINGS {
            let source = b"identity";
            let result = transcode(source, encoding, encoding).unwrap();
            if encoding == Encoding::Utf16Le {
                // `source` is treated as raw UTF-16LE code units, so identity is
                // not a byte-for-byte passthrough of ASCII text; just check it
                // succeeds (covered by `every_pair_round_trips_ascii_text`).
                continue;
            }
            assert_eq!(result, source);
        }
    }

    #[test]
    fn unmappable_characters_become_question_marks() {
        let _harness = init_icu();
        // '☕' (U+2615, HOT BEVERAGE) has no representation in ASCII or
        // LATIN1/windows-1252 -- the same character the existing C++ path is
        // exercised with in `buffer-nodejs-test.js`'s `transcodeTest`.
        let source = "☕".as_bytes();
        assert_eq!(
            transcode(source, Encoding::Utf8, Encoding::Ascii).unwrap(),
            b"?"
        );
        assert_eq!(
            transcode(source, Encoding::Utf8, Encoding::Latin1).unwrap(),
            b"?"
        );
    }

    #[test]
    fn ascii_to_utf16le_widens_high_bytes_via_latin1_path() {
        let _harness = init_icu();
        // R8: ASCII -> UTF16LE takes the latin1 path, so a high byte is
        // widened to U+00FF rather than substituted with '?'.
        let result = transcode(&[0xff], Encoding::Ascii, Encoding::Utf16Le).unwrap();
        assert_eq!(result, vec![0xff, 0x00]);
    }

    #[test]
    fn utf8_continuation_byte_only_input_yields_empty_utf16le() {
        let _harness = init_icu();
        // R8: the UTF8 -> UTF16LE size estimate is zero for input consisting
        // only of continuation bytes, so the result is empty rather than an
        // error, even though the input is non-empty.
        let result = transcode(&[0x80], Encoding::Utf8, Encoding::Utf16Le).unwrap();
        assert!(result.is_empty());
    }

    #[test]
    fn invalid_utf8_to_utf16le_is_unable_to_transcode() {
        let _harness = init_icu();
        let result = transcode(&[0x61, 0xc3], Encoding::Utf8, Encoding::Utf16Le);
        assert_eq!(result, Err(TranscodeError::UnableToTranscode));
    }

    #[test]
    fn odd_length_utf16le_to_utf8_is_rejected() {
        let _harness = init_icu();
        let result = transcode(&[0x61], Encoding::Utf16Le, Encoding::Utf8);
        assert_eq!(result, Err(TranscodeError::OddUtf16leInput));
    }

    #[test]
    fn odd_length_utf16le_to_latin1_is_rejected() {
        let _harness = init_icu();
        let result = transcode(&[0x61], Encoding::Utf16Le, Encoding::Latin1);
        assert_eq!(result, Err(TranscodeError::OddUtf16leInput));
    }

    #[test]
    fn unpaired_surrogate_utf16le_to_utf8() {
        let _harness = init_icu();
        // U+D800, an unpaired high surrogate, encoded as UTF-16LE bytes.
        let source = [0x00, 0xd8];
        // simdutf treats this as invalid input; whatever it decides (empty
        // output or an error) must not panic, and must match what the C++
        // path's `simdutf::utf8_length_from_utf16le` /
        // `simdutf::convert_utf16le_to_utf8` pair would produce, since both
        // implementations call the exact same simdutf functions.
        let result = transcode(&source, Encoding::Utf16Le, Encoding::Utf8);
        match result {
            Ok(bytes) => assert!(bytes.is_empty()),
            Err(err) => assert_eq!(err, TranscodeError::Utf8LengthMismatch),
        }
    }
}
