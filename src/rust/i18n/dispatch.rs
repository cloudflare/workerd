// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Encoding-pair dispatch, and the conversion logic behind each pair.
//!
//! A transcode runs in two steps. [`Transcoder::new`] validates the source,
//! picks the conversion, and computes [`Transcoder::dest_len`] -- the exact
//! size of the destination buffer that conversion needs.
//! [`Transcoder::transcode_into`] then fills a buffer of that size and reports
//! how many bytes it actually wrote, which is often fewer because the
//! destination is sized for the worst case.
//!
//! All sizing, validation, substitute-character setup, and length checking
//! happens here; the ICU and simdutf wrappers in [`crate::ffi`] only provide
//! the conversions themselves.

use crate::error::TranscodeError;
use crate::ffi;
use crate::ffi::Converter;
use crate::ffi::Encoding;
use crate::ffi::Utf8Source;
use crate::ffi::Utf16LeSource;

/// An isolate has a 128 MiB memory limit, and thus so does any single
/// destination buffer.
const ISOLATE_LIMIT: usize = 134_217_728;

/// Fails with `error` unless `limit` is within [`ISOLATE_LIMIT`].
///
/// Callers compute `limit` with saturating arithmetic, so a size that would
/// overflow saturates and fails here.
const fn require_within_isolate_limit(
    limit: usize,
    error: TranscodeError,
) -> Result<(), TranscodeError> {
    if limit <= ISOLATE_LIMIT {
        Ok(())
    } else {
        Err(error)
    }
}

/// A validated, sized transcode, ready to run.
///
/// Borrows its source for the whole of its life, so [`Transcoder::dest_len`]
/// cannot go stale: the bytes it was computed from are the same bytes
/// [`Transcoder::transcode_into`] reads. The UTF-8/UTF-16 conversions check
/// that they wrote exactly the length estimated from the source, so a stale
/// estimate would turn into a spurious length-mismatch error.
pub struct Transcoder<'a> {
    source: &'a [u8],
    conversion: Conversion<'a>,
    dest_len: usize,
}

/// The conversion [`Transcoder::transcode_into`] will perform, along with any
/// ICU converters or measured simdutf sources [`Transcoder::new`] needed to
/// size the destination.
enum Conversion<'a> {
    /// ICU `ucnv_convertEx` between two converters. Handles every pair the
    /// conversions below do not, including all four identity pairs.
    ConvertEx { to: Converter, from: Converter },
    /// Latin-1 to UTF-16LE, via simdutf.
    Latin1ToUtf16,
    /// ICU `ucnv_fromUChars` from UTF-16LE into ASCII or Latin-1.
    FromUtf16 { to: Converter },
    /// UTF-8 to UTF-16LE, via simdutf.
    Utf16FromUtf8(Utf8Source<'a>),
    /// UTF-16LE to UTF-8, via simdutf.
    Utf8FromUtf16(Utf16LeSource<'a>),
}

impl<'a> Transcoder<'a> {
    /// Prepares a transcode of `source` from `from` to `to`, choosing the
    /// conversion for that encoding pair.
    ///
    /// Returns an error if `from` is not a transcodable encoding, if `source`
    /// is malformed for `from`, or if the destination the conversion would
    /// need exceeds [`ISOLATE_LIMIT`].
    pub fn new(source: &'a [u8], from: Encoding, to: Encoding) -> Result<Self, TranscodeError> {
        match from {
            Encoding::Ascii | Encoding::Latin1 => {
                if to == Encoding::Utf16Le {
                    Self::transcode_latin1_to_utf16(source)
                } else {
                    Self::transcode_default(source, from, to)
                }
            }
            Encoding::Utf8 => {
                if to == Encoding::Utf16Le {
                    Self::transcode_utf16_from_utf8(source)
                } else {
                    Self::transcode_default(source, from, to)
                }
            }
            Encoding::Utf16Le => match to {
                Encoding::Utf16Le => Self::transcode_default(source, from, to),
                Encoding::Utf8 => Self::transcode_utf8_from_utf16(source),
                _ => Self::transcode_from_utf16(source, to),
            },
            _ => Err(TranscodeError::InvalidEncoding),
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
        // The conversions below refuse a short destination or stop at its end
        // rather than overflowing it, but the result would be wrong.
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
                ffi::convert_ex(to, from, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Latin1ToUtf16 => {
                // Every byte is valid Latin-1 and widens to exactly one UTF-16
                // code unit, and `dest` was sized as two bytes per source byte.
                // simdutf returns 0 only for an empty source, handled above, so
                // there is no failure result to check for.
                let units = ffi::convert_latin1_to_utf16(source, dest)
                    .ok_or(TranscodeError::DestinationSizeMismatch)?;
                Ok(units * size_of::<u16>())
            }
            Conversion::FromUtf16 { to } => {
                ffi::from_uchars(to, source, dest).ok_or(TranscodeError::UnableToTranscode)
            }
            Conversion::Utf16FromUtf8(utf8) => {
                // `dest` was sized as two bytes per estimated code unit.
                let expected_units = utf8.utf16_len();
                let units = utf8
                    .convert_to_utf16le(dest)
                    .ok_or(TranscodeError::DestinationSizeMismatch)?;
                // 0 means invalid UTF-8 input.
                if units == 0 {
                    return Err(TranscodeError::UnableToTranscode);
                }
                if units != expected_units {
                    return Err(TranscodeError::Utf16LengthMismatch);
                }
                Ok(dest.len())
            }
            Conversion::Utf8FromUtf16(utf16) => {
                let expected_bytes = utf16.utf8_len();
                let written = utf16
                    .convert_to_utf8(dest)
                    .ok_or(TranscodeError::DestinationSizeMismatch)?;
                // Invalid input writes 0 bytes, which fails this check because
                // `expected_bytes` is nonzero here. Invalid input therefore
                // surfaces as a length mismatch rather than "Unable to
                // transcode buffer", unlike every other pair.
                if written != expected_bytes {
                    return Err(TranscodeError::Utf8LengthMismatch);
                }
                Ok(written)
            }
        }
    }

    /// ICU `ucnv_convertEx` between two converters, substituting `?` for
    /// unmappable characters, sized at `to`'s maximum bytes per character for
    /// every source byte.
    fn transcode_default(
        source: &'a [u8],
        from: Encoding,
        to: Encoding,
    ) -> Result<Self, TranscodeError> {
        let to_conv = Converter::open(to)?;
        to_conv.set_subst_chars(&"?".repeat(to_conv.min_char_size()))?;
        let from_conv = Converter::open(from)?;

        let limit = source.len().saturating_mul(to_conv.max_char_size());
        require_within_isolate_limit(limit, TranscodeError::SourceBufferTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::ConvertEx {
                to: to_conv,
                from: from_conv,
            },
            dest_len: limit,
        })
    }

    /// Widening of ASCII/Latin-1 into UTF-16LE.
    ///
    /// Taken for an `Ascii` source as well as a `Latin1` one, so source bytes
    /// `0x80`-`0xFF` widen to U+0080-U+00FF instead of being substituted.
    fn transcode_latin1_to_utf16(source: &'a [u8]) -> Result<Self, TranscodeError> {
        let length_in_chars = source.len().saturating_mul(2);
        require_within_isolate_limit(length_in_chars, TranscodeError::SourceBufferTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::Latin1ToUtf16,
            dest_len: length_in_chars,
        })
    }

    /// ICU `ucnv_fromUChars` from UTF-16LE into ASCII or Latin-1, substituting
    /// `?` for unmappable characters, sized at `to`'s maximum bytes per
    /// character for every code unit.
    fn transcode_from_utf16(source: &'a [u8], to: Encoding) -> Result<Self, TranscodeError> {
        let to_conv = Converter::open(to)?;
        to_conv.set_subst_chars(&"?".repeat(to_conv.min_char_size()))?;

        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let limit = (source.len() / 2).saturating_mul(to_conv.max_char_size());
        require_within_isolate_limit(limit, TranscodeError::BufferTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::FromUtf16 { to: to_conv },
            dest_len: limit,
        })
    }

    /// UTF-8 to UTF-16LE, sized from [`Utf8Source::utf16_len`].
    ///
    /// That estimate is zero for some non-empty inputs -- a source of nothing
    /// but UTF-8 continuation bytes, for instance -- which yields an empty
    /// result rather than an error.
    fn transcode_utf16_from_utf8(source: &'a [u8]) -> Result<Self, TranscodeError> {
        let utf8 = Utf8Source::new(source);
        let expected_utf16_length = utf8.utf16_len();
        require_within_isolate_limit(
            expected_utf16_length,
            TranscodeError::ExpectedUtf16LengthTooLarge,
        )?;

        Ok(Self {
            source,
            conversion: Conversion::Utf16FromUtf8(utf8),
            // Cannot overflow: at most `ISOLATE_LIMIT` code units.
            dest_len: expected_utf16_length * size_of::<u16>(),
        })
    }

    /// UTF-16LE to UTF-8, sized from [`Utf16LeSource::utf8_len`].
    fn transcode_utf8_from_utf16(source: &'a [u8]) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let utf16 = Utf16LeSource::new(source);
        let expected_utf8_length = utf16.utf8_len();
        require_within_isolate_limit(
            expected_utf8_length,
            TranscodeError::ExpectedUtf8LengthTooLarge,
        )?;

        Ok(Self {
            source,
            conversion: Conversion::Utf8FromUtf16(utf16),
            dest_len: expected_utf8_length,
        })
    }
}

#[cfg(test)]
#[path = "dispatch-test.rs"]
mod tests;
