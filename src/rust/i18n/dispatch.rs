// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Encoding-pair dispatch and the conversion logic behind each pair, ported
//! from `i18n.c++`'s `TranscodeDefault` / `TranscodeLatin1ToUTF16` /
//! `TranscodeFromUTF16` / `TranscodeUTF16FromUTF8` / `TranscodeUTF8FromUTF16`
//! and the `switch` in `transcode()` that picks between them.
//!
//! A transcode runs in two steps. [`Transcoder::new`] validates the source,
//! picks the conversion, and computes [`Transcoder::dest_len`] -- the exact
//! size of the destination buffer that conversion needs.
//! [`Transcoder::transcode_into`] then fills a buffer of that size and reports
//! how many bytes it actually wrote, which is often fewer because the
//! destination is sized for the worst case.
//!
//! All sizing, validation, substitute-character setup, and length checking
//! happens here; [`crate::codecs`] only forwards to the underlying ICU and
//! simdutf calls.

use crate::codecs;
use crate::codecs::Converter;
use crate::error::TranscodeError;
use crate::ffi::Encoding;

/// An isolate has a 128MB memory limit, and thus so does any single
/// destination buffer. Mirrors `ISOLATE_LIMIT` in `i18n.c++`.
const ISOLATE_LIMIT: usize = 134_217_728;

/// A validated, sized transcode, ready to run.
///
/// Borrows its source for the whole of its life, so [`Transcoder::dest_len`]
/// cannot go stale: the bytes it was computed from are the same bytes
/// [`Transcoder::transcode_into`] reads. This matters because three of the
/// five conversions bottom out in simdutf functions that take no output
/// length and size their writes purely from the source.
pub struct Transcoder<'a> {
    source: &'a [u8],
    conversion: Conversion,
    dest_len: usize,
}

/// The conversion [`Transcoder::transcode_into`] will perform, along with any
/// ICU converters [`Transcoder::new`] had to open to size the destination.
enum Conversion {
    /// ICU `ucnv_convertEx` between two converters, mirroring
    /// `TranscodeDefault`. Handles every pair the simdutf conversions below do
    /// not, including all four identity pairs.
    ConvertEx { to: Converter, from: Converter },
    /// simdutf Latin-1 to UTF-16, mirroring `TranscodeLatin1ToUTF16`.
    Latin1ToUtf16,
    /// ICU `ucnv_fromUChars` from UTF-16LE, mirroring `TranscodeFromUTF16`.
    FromUtf16 { to: Converter },
    /// simdutf UTF-8 to UTF-16LE, mirroring `TranscodeUTF16FromUTF8`.
    Utf16FromUtf8,
    /// simdutf UTF-16LE to UTF-8, mirroring `TranscodeUTF8FromUTF16`.
    Utf8FromUtf16,
}

impl<'a> Transcoder<'a> {
    /// Prepares a transcode of `source` from `from` to `to`, matching the
    /// dispatch table built by `i18n::transcode()` in `i18n.c++`.
    ///
    /// Returns an error if `source` is malformed for `from`, or if the
    /// destination the conversion would need exceeds [`ISOLATE_LIMIT`].
    pub fn new(source: &'a [u8], from: Encoding, to: Encoding) -> Result<Self, TranscodeError> {
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

    /// Transcodes into `dest`, returning the number of bytes written, which
    /// may be fewer than `dest.len()`.
    ///
    /// `dest` must be exactly [`Transcoder::dest_len`] bytes long.
    pub fn transcode_into(&self, dest: &mut [u8]) -> Result<usize, TranscodeError> {
        // Not merely a sanity check: the simdutf conversions below take no
        // output length, so a destination shorter than the size computed for
        // this source would overflow it.
        if dest.len() != self.dest_len {
            return Err(TranscodeError::DestinationSizeMismatch);
        }
        // A zero-length destination means the conversion has nothing to write:
        // either the source was empty, or the estimated output length was zero.
        if dest.is_empty() {
            return Ok(0);
        }

        let source = self.source;
        match &self.conversion {
            Conversion::ConvertEx { to, from } => {
                codecs::convert_ex(to, from, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Latin1ToUtf16 => {
                let units = codecs::convert_latin1_to_utf16(source, dest);
                // simdutf returns 0 for invalid input.
                if units == 0 {
                    return Err(TranscodeError::UnableToTranscode);
                }
                // Each Latin-1 byte widens to exactly one UTF-16 code unit, and
                // `dest` was sized as two bytes per source byte.
                Ok(units * 2)
            }
            Conversion::FromUtf16 { to } => {
                codecs::from_uchars(to, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Utf16FromUtf8 => {
                // `dest` was sized as two bytes per estimated code unit.
                let expected_units = dest.len() / 2;
                let units = codecs::convert_utf8_to_utf16le(source, dest);
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
                let written = codecs::convert_utf16le_to_utf8(source, dest);
                // simdutf returns 0 for invalid input, which fails this check
                // because `expected_bytes` is nonzero here. The C++
                // `TranscodeUTF8FromUTF16` checks for 0 only *after* requiring
                // equality with the (nonzero) estimate, so that branch is dead
                // there too: a simdutf failure surfaces as a length mismatch,
                // not "Unable to transcode buffer", unlike every other pair.
                if written != expected_bytes {
                    return Err(TranscodeError::Utf8LengthMismatch);
                }
                Ok(written)
            }
        }
    }

    /// ICU `ucnv_convertEx` between two converters, sized at `to`'s maximum
    /// bytes per character.
    fn convert_ex(source: &'a [u8], from: Encoding, to: Encoding) -> Result<Self, TranscodeError> {
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
            source,
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
    fn latin1_to_utf16(source: &'a [u8]) -> Result<Self, TranscodeError> {
        let dest_len = source
            .len()
            .checked_mul(2)
            .ok_or(TranscodeError::SourceBufferTooLarge)?;
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::SourceBufferTooLarge);
        }

        Ok(Self {
            source,
            conversion: Conversion::Latin1ToUtf16,
            dest_len,
        })
    }

    /// ICU `ucnv_fromUChars` from UTF-16LE into `to`'s encoding, sized at
    /// `to`'s maximum bytes per character.
    fn from_utf16(source: &'a [u8], to: Encoding) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let to_conv = Converter::open(to)?;
        to_conv.set_subst_chars(&"?".repeat(to_conv.min_char_size()))?;

        let dest_len = (source.len() / 2)
            .checked_mul(to_conv.max_char_size())
            .ok_or(TranscodeError::BufferTooLarge)?;
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::BufferTooLarge);
        }

        Ok(Self {
            source,
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
    fn utf16_from_utf8(source: &'a [u8]) -> Result<Self, TranscodeError> {
        let expected_units = codecs::utf16_length_from_utf8(source);
        if expected_units > ISOLATE_LIMIT {
            return Err(TranscodeError::ExpectedUtf16LengthTooLarge);
        }
        let dest_len = expected_units
            .checked_mul(2)
            .ok_or(TranscodeError::ExpectedUtf16LengthTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::Utf16FromUtf8,
            dest_len,
        })
    }

    /// simdutf UTF-16LE to UTF-8, sized from
    /// `simdutf::utf8_length_from_utf16le`.
    fn utf8_from_utf16(source: &'a [u8]) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let dest_len = codecs::utf8_length_from_utf16le(source);
        if dest_len > ISOLATE_LIMIT {
            return Err(TranscodeError::ExpectedUtf8LengthTooLarge);
        }

        Ok(Self {
            source,
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

    /// Runs both transcode steps the way [`crate::transcode`] does -- size,
    /// allocate, convert, narrow to the written length -- against a plain
    /// `Vec` rather than a V8 backing store, and returns the written bytes.
    fn transcode(source: &[u8], from: Encoding, to: Encoding) -> Result<Vec<u8>, TranscodeError> {
        let transcoder = Transcoder::new(source, from, to)?;
        let mut dest = vec![0u8; transcoder.dest_len()];
        let written = transcoder.transcode_into(&mut dest)?;
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

    #[test]
    fn destination_size_mismatch_is_rejected() {
        let _harness = init_icu();
        let transcoder = Transcoder::new(b"Hi", Encoding::Latin1, Encoding::Utf16Le).unwrap();
        let mut too_small = vec![0u8; transcoder.dest_len() - 1];
        assert_eq!(
            transcoder.transcode_into(&mut too_small),
            Err(TranscodeError::DestinationSizeMismatch)
        );
    }
}
