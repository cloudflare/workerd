// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Encoding-pair dispatch and the conversion logic behind each pair.
//!
//! A transcode runs in two steps. [`Transcoder::new`] validates `source`,
//! picks the conversion to perform, and computes
//! [`Transcoder::dest_len`] -- the exact size of the destination buffer that
//! conversion needs. The caller allocates a buffer of that size and passes it
//! to [`Transcoder::transcode_into`], which fills it and returns the number of
//! bytes actually written.
//!
//! The destination is sized for the worst case, so the write is often shorter
//! than the buffer; the caller is expected to narrow its view of the buffer to
//! the returned length rather than shrink the buffer itself.
//!
//! All sizing, validation, substitute-character setup, and length checking
//! happens here; [`crate::shim`] only forwards to the underlying ICU and
//! simdutf calls.

use crate::error::TranscodeError;
use crate::ffi::Encoding;
use crate::shim;
use crate::shim::Converter;

/// The memory limit of an isolate, and thus the ceiling on any single
/// destination buffer. Conversions are rejected rather than attempted above
/// this size.
const ISOLATE_LIMIT: usize = 128 * 1024 * 1024;

/// A validated, sized transcode, ready to run.
pub struct Transcoder {
    conversion: Conversion,
    dest_len: usize,
}

/// The conversion [`Transcoder::transcode_into`] will perform, along with any
/// ICU converters [`Transcoder::new`] had to open to size the destination.
enum Conversion {
    /// ICU `ucnv_convertEx` between two converters. Handles every pair the
    /// simdutf conversions below do not, including all four identity pairs.
    ConvertEx { to: Converter, from: Converter },
    /// simdutf Latin-1 to UTF-16.
    Latin1ToUtf16,
    /// ICU `ucnv_fromUChars` from UTF-16LE.
    FromUtf16 { to: Converter },
    /// simdutf UTF-8 to UTF-16LE.
    Utf16FromUtf8,
    /// simdutf UTF-16LE to UTF-8.
    Utf8FromUtf16,
}

impl Transcoder {
    /// Prepares a transcode of `source` from `from` to `to`.
    ///
    /// Returns an error if `source` is malformed for `from`, or if the
    /// destination the conversion would need exceeds [`ISOLATE_LIMIT`].
    pub fn new(source: &[u8], from: Encoding, to: Encoding) -> Result<Self, TranscodeError> {
        match (from, to) {
            (Encoding::Ascii | Encoding::Latin1, Encoding::Utf16Le) => {
                Self::latin1_to_utf16(source)
            }
            (Encoding::Utf8, Encoding::Utf16Le) => Self::utf16_from_utf8(source),
            (Encoding::Utf16Le, Encoding::Utf8) => Self::utf8_from_utf16(source),
            (Encoding::Utf16Le, Encoding::Ascii | Encoding::Latin1) => Self::from_utf16(source, to),
            // Identity pairs, UTF16LE -> UTF16LE, and anything else the
            // simdutf conversions above do not cover.
            _ => Self::convert_ex(source, from, to),
        }
    }

    /// The exact size, in bytes, of the destination buffer
    /// [`Transcoder::transcode_into`] requires.
    pub fn dest_len(&self) -> usize {
        self.dest_len
    }

    /// Transcodes `source` into `dest`, returning the number of bytes written.
    ///
    /// `source` must be the buffer this transcoder was built from, and `dest`
    /// must be exactly [`Transcoder::dest_len`] bytes long. The written length
    /// is always less than or equal to `dest.len()`.
    pub fn transcode_into(&self, source: &[u8], dest: &mut [u8]) -> Result<usize, TranscodeError> {
        if dest.len() != self.dest_len {
            return Err(TranscodeError::DestinationSizeMismatch);
        }
        // A zero-length destination means the conversion has nothing to write:
        // either the source was empty, or the estimated output length was zero.
        if dest.is_empty() {
            return Ok(0);
        }

        match &self.conversion {
            Conversion::ConvertEx { to, from } => {
                shim::convert_ex(to, from, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Latin1ToUtf16 => {
                let units = shim::convert_latin1_to_utf16(source, dest);
                // simdutf returns 0 for invalid input.
                if units == 0 {
                    return Err(TranscodeError::UnableToTranscode);
                }
                // Each Latin-1 byte widens to exactly one UTF-16 code unit, and
                // `dest` was sized as two bytes per source byte.
                Ok(units * 2)
            }
            Conversion::FromUtf16 { to } => {
                shim::from_uchars(to, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Utf16FromUtf8 => {
                // `dest` was sized as two bytes per estimated code unit.
                let expected_units = dest.len() / 2;
                let units = shim::convert_utf8_to_utf16le(source, dest);
                // simdutf returns 0 for invalid UTF-8 input.
                if units == 0 {
                    return Err(TranscodeError::UnableToTranscode);
                }
                if units != expected_units {
                    return Err(TranscodeError::Utf16LengthMismatch);
                }
                Ok(dest.len())
            }
            Conversion::Utf8FromUtf16 => {
                let expected_bytes = dest.len();
                let written = shim::convert_utf16le_to_utf8(source, dest);
                // simdutf returns 0 for invalid input, which fails this check
                // because `expected_bytes` is nonzero here.
                if written != expected_bytes {
                    return Err(TranscodeError::Utf8LengthMismatch);
                }
                Ok(written)
            }
        }
    }

    /// ICU `ucnv_convertEx` between two converters, sized at `to`'s maximum
    /// bytes per character.
    fn convert_ex(source: &[u8], from: Encoding, to: Encoding) -> Result<Self, TranscodeError> {
        let to_conv = Converter::open(to)?;
        to_conv.set_subst_chars(&"?".repeat(to_conv.min_char_size()))?;
        let from_conv = Converter::open(from)?;

        let dest_len = source
            .len()
            .checked_mul(to_conv.max_char_size())
            .ok_or(TranscodeError::SourceBufferTooLarge)?;
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::SourceBufferTooLarge);
        }

        Ok(Self {
            conversion: Conversion::ConvertEx {
                to: to_conv,
                from: from_conv,
            },
            dest_len,
        })
    }

    /// simdutf widening of ASCII/Latin-1 into UTF-16.
    ///
    /// Taken for an `Ascii` source as well as a `Latin1` one, so source bytes
    /// `0x80`-`0xFF` widen to U+0080-U+00FF instead of being substituted.
    ///
    /// No ICU converter is involved: the widening is purely arithmetic.
    fn latin1_to_utf16(source: &[u8]) -> Result<Self, TranscodeError> {
        let dest_len = source
            .len()
            .checked_mul(2)
            .ok_or(TranscodeError::SourceBufferTooLarge)?;
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::SourceBufferTooLarge);
        }

        Ok(Self {
            conversion: Conversion::Latin1ToUtf16,
            dest_len,
        })
    }

    /// ICU `ucnv_fromUChars` from UTF-16LE into `to`'s encoding, sized at
    /// `to`'s maximum bytes per character.
    fn from_utf16(source: &[u8], to: Encoding) -> Result<Self, TranscodeError> {
        let to_conv = Converter::open(to)?;
        to_conv.set_subst_chars(&"?".repeat(to_conv.min_char_size()))?;

        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let dest_len = (source.len() / 2)
            .checked_mul(to_conv.max_char_size())
            .ok_or(TranscodeError::BufferTooLarge)?;
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::BufferTooLarge);
        }

        Ok(Self {
            conversion: Conversion::FromUtf16 { to: to_conv },
            dest_len,
        })
    }

    /// simdutf UTF-8 to UTF-16LE, sized from
    /// `simdutf::utf16_length_from_utf8`.
    ///
    /// That estimate is zero for some non-empty inputs -- a source of nothing
    /// but UTF-8 continuation bytes, for instance -- which yields an empty
    /// result rather than an error.
    fn utf16_from_utf8(source: &[u8]) -> Result<Self, TranscodeError> {
        let expected_units = shim::utf16_length_from_utf8(source);
        if expected_units > ISOLATE_LIMIT {
            return Err(TranscodeError::ExpectedUtf16LengthTooLarge);
        }
        let dest_len = expected_units
            .checked_mul(2)
            .ok_or(TranscodeError::ExpectedUtf16LengthTooLarge)?;

        Ok(Self {
            conversion: Conversion::Utf16FromUtf8,
            dest_len,
        })
    }

    /// simdutf UTF-16LE to UTF-8, sized from
    /// `simdutf::utf8_length_from_utf16le`.
    fn utf8_from_utf16(source: &[u8]) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let dest_len = shim::utf8_length_from_utf16le(source);
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::ExpectedUtf8LengthTooLarge);
        }

        Ok(Self {
            conversion: Conversion::Utf8FromUtf16,
            dest_len,
        })
    }
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

    /// Runs both transcode steps the way the C++ caller does -- size, allocate,
    /// convert, narrow to the written length -- and returns the written bytes.
    fn transcode(source: &[u8], from: Encoding, to: Encoding) -> Result<Vec<u8>, TranscodeError> {
        let transcoder = Transcoder::new(source, from, to)?;
        let mut dest = vec![0u8; transcoder.dest_len()];
        let written = transcoder.transcode_into(source, &mut dest)?;
        assert!(
            written <= dest.len(),
            "{from:?} -> {to:?} wrote {written} bytes into a {} byte buffer",
            dest.len()
        );
        dest.truncate(written);
        Ok(dest)
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
        // '☕' (U+2615, HOT BEVERAGE) has no representation in ASCII or Latin-1.
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
        // ASCII -> UTF16LE takes the Latin-1 path, so a high byte is widened to
        // U+00FF rather than substituted with '?'.
        let result = transcode(&[0xff], Encoding::Ascii, Encoding::Utf16Le).unwrap();
        assert_eq!(result, vec![0xff, 0x00]);
    }

    #[test]
    fn utf8_continuation_byte_only_input_yields_empty_utf16le() {
        let _harness = init_icu();
        // The UTF8 -> UTF16LE size estimate is zero for input consisting only of
        // continuation bytes, so the result is empty rather than an error, even
        // though the input is non-empty.
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
        // `simdutf::utf8_length_from_utf16le` does not validate, and reports the
        // three bytes a replacement character would occupy, but
        // `simdutf::convert_utf16le_to_utf8` rejects the input and writes
        // nothing -- so the mismatch check is what surfaces the failure.
        let result = transcode(&source, Encoding::Utf16Le, Encoding::Utf8);
        assert_eq!(result, Err(TranscodeError::Utf8LengthMismatch));
    }
}
