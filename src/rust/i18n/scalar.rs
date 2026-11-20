// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The conversions simdutf does not cover: those that substitute for
//! ill-formed input or for characters the target cannot represent. They
//! decode the source to `char`s and encode them one at a time.
//!
//! Node.js uses ICU for these. workerd's ICU can't stand in for it, because
//! Chromium's ICU data aliases `us-ascii` and `iso8859-1` to `windows-1252`,
//! which maps bytes 0x80-0x9F to other code points and accepts non-ASCII
//! bytes as ASCII. The rules here match the standard ICU converters Node.js
//! uses:
//!
//! - Latin-1 bytes decode to U+0000-U+00FF.
//! - ASCII bytes 0x80-0xFF, malformed UTF-8 (one U+FFFD per maximal subpart),
//!   unpaired UTF-16 surrogates and a trailing odd UTF-16 byte (together
//!   with a lead surrogate before it) decode to U+FFFD.
//! - Encoding into ASCII or Latin-1 writes `?` for a code point the target
//!   cannot represent, except that an unrepresentable default-ignorable code
//!   point is dropped, as ICU's substitute callback does.

use std::char::REPLACEMENT_CHARACTER;

use crate::ffi::Encoding;

/// The most bytes converting `source` from `from` to `to` can write: exact
/// for ASCII to UTF-8 and for UTF-16LE to UTF-16LE, an upper bound otherwise.
/// Saturates on overflow.
pub fn dest_len(source: &[u8], from: Encoding, to: Encoding) -> usize {
    let non_ascii = || source.iter().filter(|byte| !byte.is_ascii()).count();
    match (from, to) {
        // Each maximal subpart of ill-formed UTF-8, at least one byte, becomes
        // U+FFFD, three bytes.
        (Encoding::Utf8, Encoding::Utf8) => source.len().saturating_mul(3),
        // Each code unit stays one code unit, and a trailing odd byte becomes
        // U+FFFD, one code unit.
        (Encoding::Utf16Le, Encoding::Utf16Le) => source.len().saturating_add(1) & !1,
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
/// A UTF-16LE `source` must have even length unless `to` is UTF-16LE too.
pub fn transcode_into(source: &[u8], from: Encoding, to: Encoding, dest: &mut [u8]) -> usize {
    if from == Encoding::Utf8 && to == Encoding::Utf8 {
        return repair_utf8(source, dest);
    }
    // The highest code point the target encodes as a single byte.
    let max = if to == Encoding::Ascii { 0x7F } else { 0xFF };
    let mut written = 0;
    let mut push = |c: char| match to {
        Encoding::Utf8 => written += c.encode_utf8(&mut dest[written..]).len(),
        Encoding::Utf16Le => {
            for unit in c.encode_utf16(&mut [0; 2]) {
                dest[written..written + 2].copy_from_slice(&unit.to_le_bytes());
                written += 2;
            }
        }
        _ => {
            let byte = match u8::try_from(c) {
                Ok(byte) if byte <= max => byte,
                _ if is_default_ignorable(c) => return,
                _ => b'?',
            };
            dest[written] = byte;
            written += 1;
        }
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
            let pairs = source.chunks_exact(2);
            let truncated = !pairs.remainder().is_empty();
            let units = pairs.map(|pair| u16::from_le_bytes([pair[0], pair[1]]));
            // A trailing odd byte is a truncated code unit, and with a lead
            // surrogate before it, one truncated character: a single U+FFFD
            // either way, as ICU gives.
            let ends_in_lead = units
                .clone()
                .next_back()
                .is_some_and(|unit| (0xD800..0xDC00).contains(&unit));
            char::decode_utf16(units)
                .map(|c| c.unwrap_or(REPLACEMENT_CHARACTER))
                .for_each(&mut push);
            if truncated && !ends_in_lead {
                push(REPLACEMENT_CHARACTER);
            }
        }
    }
    written
}

/// Copies `source` into `dest`, with each maximal subpart of ill-formed UTF-8
/// replaced by U+FFFD.
fn repair_utf8(source: &[u8], dest: &mut [u8]) -> usize {
    let mut written = 0;
    let mut write = |bytes: &[u8]| {
        dest[written..written + bytes.len()].copy_from_slice(bytes);
        written += bytes.len();
    };
    for chunk in source.utf8_chunks() {
        write(chunk.valid().as_bytes());
        if !chunk.invalid().is_empty() {
            write("\u{fffd}".as_bytes());
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
#[path = "scalar-test.rs"]
mod tests;
