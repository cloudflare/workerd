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
//! All sizing, validation, and length checking happens here;
//! [`crate::codecs`] only provides the conversions themselves.

use crate::codecs;
use crate::error::TranscodeError;
use crate::ffi::Encoding;

/// An isolate has a 128MB memory limit, and thus so does any single
/// destination buffer. Mirrors `ISOLATE_LIMIT` in `i18n.c++`.
const ISOLATE_LIMIT: usize = 134_217_728;

/// Fails with `error` unless `limit` is within [`ISOLATE_LIMIT`], mirroring
/// `i18n.c++`'s `JSG_REQUIRE(limit <= ISOLATE_LIMIT, Error, ...)`.
///
/// Callers compute `limit` with saturating arithmetic, where the C++ path
/// multiplies unchecked, so an overflowing size saturates and fails here.
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
    conversion: Conversion,
    dest_len: usize,
}

/// The conversion [`Transcoder::transcode_into`] will perform.
enum Conversion {
    /// [`codecs::transcode_substituting`], mirroring the ICU conversions in
    /// `TranscodeDefault` and `TranscodeFromUTF16`. Handles every pair the
    /// conversions below do not, including all four identity pairs.
    Icu { from: Encoding, to: Encoding },
    /// Latin-1 to UTF-16, mirroring `TranscodeLatin1ToUTF16`.
    Latin1ToUtf16,
    /// UTF-8 to UTF-16LE, mirroring `TranscodeUTF16FromUTF8`.
    Utf16FromUtf8,
    /// UTF-16LE to UTF-8, mirroring `TranscodeUTF8FromUTF16`.
    Utf8FromUtf16,
}

impl<'a> Transcoder<'a> {
    /// Prepares a transcode of `source` from `from` to `to`, matching the
    /// dispatch table built by `i18n::transcode()` in `i18n.c++`.
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
        // The conversions below stop at the end of a short destination rather
        // than overflowing it, but the result would be silently truncated.
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
            // Substitutes rather than failing. The C++ path reports "Unable to
            // transcode buffer" if ICU fails, which these converters do not do
            // for a destination sized as below.
            &Conversion::Icu { from, to } => {
                Ok(codecs::transcode_substituting(from, to, source, dest))
            }
            Conversion::Latin1ToUtf16 => {
                // Every byte is valid Latin-1 and widens to exactly one UTF-16
                // code unit, and `dest` was sized as two bytes per source byte.
                // The C++ path checks simdutf's result for 0, which it can only
                // return for an empty source, handled above.
                Ok(codecs::convert_latin1_to_utf16(source, dest) * 2)
            }
            Conversion::Utf16FromUtf8 => {
                // `dest` was sized as two bytes per estimated code unit.
                let expected_units = dest.len() / 2;
                let units = codecs::convert_utf8_to_utf16le(source, dest);
                // 0 means invalid UTF-8 input.
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
                // Invalid input writes 0 bytes, which fails this check because
                // `expected_bytes` is nonzero here. The C++
                // `TranscodeUTF8FromUTF16` checks for 0 only *after* requiring
                // equality with the (nonzero) estimate, so that branch is dead
                // there too: invalid input surfaces as a length mismatch, not
                // "Unable to transcode buffer", unlike every other pair.
                if written != expected_bytes {
                    return Err(TranscodeError::Utf8LengthMismatch);
                }
                Ok(written)
            }
        }
    }

    /// Mirrors `TranscodeDefault`: a substituting conversion, as ICU's
    /// `ucnv_convertEx` performs, sized at `to`'s maximum bytes per character
    /// for every source byte.
    ///
    /// Always large enough: no source byte produces more than that. A byte is
    /// at most one character, and for a UTF-8 destination, the worst case is
    /// a single byte becoming a three-byte character: U+FFFD for an ill-formed
    /// UTF-8 byte, or, say, the euro sign for windows-1252 byte `0x80`.
    fn transcode_default(
        source: &'a [u8],
        from: Encoding,
        to: Encoding,
    ) -> Result<Self, TranscodeError> {
        let limit = source.len().saturating_mul(codecs::max_char_size(to));
        require_within_isolate_limit(limit, TranscodeError::SourceBufferTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::Icu { from, to },
            dest_len: limit,
        })
    }

    /// Mirrors `TranscodeLatin1ToUTF16`: widening of ASCII/Latin-1 into
    /// UTF-16.
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

    /// Mirrors `TranscodeFromUTF16`: a substituting conversion from UTF-16LE
    /// into ASCII or Latin-1, as ICU's `ucnv_fromUChars` performs, sized at
    /// `to`'s maximum bytes per character for every code unit. Each code unit
    /// is at most one character, so one byte, which always fits.
    fn transcode_from_utf16(source: &'a [u8], to: Encoding) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let limit = (source.len() / 2).saturating_mul(codecs::max_char_size(to));
        require_within_isolate_limit(limit, TranscodeError::BufferTooLarge)?;

        Ok(Self {
            source,
            conversion: Conversion::Icu {
                from: Encoding::Utf16Le,
                to,
            },
            dest_len: limit,
        })
    }

    /// Mirrors `TranscodeUTF16FromUTF8`: UTF-8 to UTF-16LE, sized from
    /// [`codecs::utf16_length_from_utf8`].
    ///
    /// That estimate is zero for some non-empty inputs -- a source of nothing
    /// but UTF-8 continuation bytes, for instance -- which yields an empty
    /// result rather than an error.
    fn transcode_utf16_from_utf8(source: &'a [u8]) -> Result<Self, TranscodeError> {
        let expected_utf16_length = codecs::utf16_length_from_utf8(source);
        require_within_isolate_limit(
            expected_utf16_length,
            TranscodeError::ExpectedUtf16LengthTooLarge,
        )?;

        Ok(Self {
            source,
            conversion: Conversion::Utf16FromUtf8,
            // Cannot overflow: at most `ISOLATE_LIMIT` code units.
            dest_len: expected_utf16_length * 2,
        })
    }

    /// Mirrors `TranscodeUTF8FromUTF16`: UTF-16LE to UTF-8, sized from
    /// [`codecs::utf8_length_from_utf16le`].
    fn transcode_utf8_from_utf16(source: &'a [u8]) -> Result<Self, TranscodeError> {
        if !source.len().is_multiple_of(2) {
            return Err(TranscodeError::OddUtf16leInput);
        }

        let expected_utf8_length = codecs::utf8_length_from_utf16le(source);
        require_within_isolate_limit(
            expected_utf8_length,
            TranscodeError::ExpectedUtf8LengthTooLarge,
        )?;

        Ok(Self {
            source,
            conversion: Conversion::Utf8FromUtf16,
            dest_len: expected_utf8_length,
        })
    }
}

#[cfg(test)]
#[path = "dispatch-test.rs"]
mod tests;
