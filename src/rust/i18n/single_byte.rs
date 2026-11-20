// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Conversions that have ASCII or Latin-1 on either side, apart from those
//! `dispatch` leaves to simdutf: widening into UTF-16LE, Latin-1 to UTF-8, and
//! leading ASCII bytes, which are the same in every encoding here and copied.
//!
//! These do not use ICU: Chromium's ICU data aliases `us-ascii` and
//! `iso8859-1` to `windows-1252`, which maps bytes 0x80-0x9F to other code
//! points and accepts non-ASCII bytes as ASCII. The rules here match the
//! standard ICU converters Node.js uses:
//!
//! - Latin-1 bytes decode to U+0000-U+00FF.
//! - ASCII bytes 0x80-0xFF, malformed UTF-8 (one U+FFFD per maximal subpart),
//!   and unpaired UTF-16 surrogates decode to U+FFFD.
//! - Encoding into ASCII or Latin-1 writes `?` for a code point the target
//!   cannot represent, except that an unrepresentable default-ignorable code
//!   point is dropped, as ICU's substitute callback does.

use std::char::REPLACEMENT_CHARACTER;

use crate::ffi::Encoding;

/// The most bytes converting `source` from `from` to `to` can write: exact
/// for ASCII to UTF-8, an upper bound otherwise. Saturates on overflow.
pub fn dest_len(source: &[u8], from: Encoding, to: Encoding) -> usize {
    let non_ascii = || source.iter().filter(|byte| !byte.is_ascii()).count();
    match (from, to) {
        // At most one output byte per code unit.
        (Encoding::Utf16Le, _) => source.len() / size_of::<u16>(),
        // A non-ASCII byte decodes to U+FFFD, three bytes in UTF-8.
        (Encoding::Ascii, Encoding::Utf8) => {
            source.len().saturating_add(non_ascii().saturating_mul(2))
        }
        // At most one output byte per source byte.
        _ => source.len(),
    }
}

/// Converts `source` from `from` to `to` into `dest`, which must be at least
/// [`dest_len`] bytes long, and returns the number of bytes written.
///
/// A UTF-16LE `source` must have even length.
pub fn transcode_into(source: &[u8], from: Encoding, to: Encoding, dest: &mut [u8]) -> usize {
    // The highest code point the target encodes as a single byte.
    let max = if to == Encoding::Ascii { 0x7F } else { 0xFF };
    let mut written = 0;
    let mut push = |c: char| {
        if to == Encoding::Utf8 {
            written += c.encode_utf8(&mut dest[written..]).len();
            return;
        }
        dest[written] = match u8::try_from(c) {
            Ok(byte) if byte <= max => byte,
            _ if is_default_ignorable(c) => return,
            _ => b'?',
        };
        written += 1;
    };
    match from {
        Encoding::Ascii | Encoding::Latin1 => source
            .iter()
            .map(|&byte| {
                if from == Encoding::Ascii && !byte.is_ascii() {
                    REPLACEMENT_CHARACTER
                } else {
                    char::from(byte)
                }
            })
            .for_each(push),
        Encoding::Utf8 => {
            for chunk in source.utf8_chunks() {
                chunk.valid().chars().for_each(&mut push);
                if !chunk.invalid().is_empty() {
                    push(REPLACEMENT_CHARACTER);
                }
            }
        }
        _ => {
            let units = source
                .chunks_exact(2)
                .map(|pair| u16::from_le_bytes([pair[0], pair[1]]));
            char::decode_utf16(units)
                .map(|c| c.unwrap_or(REPLACEMENT_CHARACTER))
                .for_each(push);
        }
    }
    written
}

/// The code points ICU's substitute callback drops rather than substitutes
/// when the target encoding cannot represent them (`IS_DEFAULT_IGNORABLE_CODE_POINT`
/// in ICU's `ucnv_err.cpp`).
fn is_default_ignorable(c: char) -> bool {
    matches!(
        u32::from(c),
        0x00AD
            | 0x034F
            | 0x061C
            | 0x115F
            | 0x1160
            | 0x17B4..=0x17B5
            | 0x180B..=0x180F
            | 0x200B..=0x200F
            | 0x202A..=0x202E
            | 0x2060..=0x206F
            | 0x3164
            | 0xFE00..=0xFE0F
            | 0xFEFF
            | 0xFFA0
            | 0xFFF0..=0xFFF8
            | 0x1BCA0..=0x1BCA3
            | 0x1D173..=0x1D17A
            | 0xE0000..=0xE0FFF
    )
}

#[cfg(test)]
#[path = "single_byte-test.rs"]
mod tests;
