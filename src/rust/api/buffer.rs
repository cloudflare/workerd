// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "views the memory of Uint8Arrays while calling V8 functions that take the lock the \
              safe slice accessors borrow"
)]

//! `node-internal:buffer_native`: the primitives behind the TypeScript `node-internal:buffer`.
//!
//! The TypeScript module is `src/node/internal/buffer.ts`, and its declarations of these
//! primitives are in `src/node/internal/buffer_native.d.ts`.
//!
//! String conversions call V8. Everything else is Rust: base64 through `base64-simd`, with the
//! two forgiving decoders of the C++ `BufferUtil` (simdutf's and nbytes') ported here; substring
//! search through `memchr`; UTF-8 validation through `simdutf8`; and `transcode` through the
//! `i18n` crate.
//!
//! Where a method operates on part of an array, it takes the array with a `[start, end)` range,
//! since a JavaScript view of the part would cost an allocation. The TypeScript module validates
//! arguments before calling in; the checks here only keep each method memory-safe on its own.

use std::mem::MaybeUninit;
use std::ops::Range;

use base64_simd::Out;
use base64_simd::STANDARD;
use base64_simd::STANDARD_NO_PAD;
use base64_simd::URL_SAFE_NO_PAD;
use jsg::JsString;
use jsg::v8;
use jsg::v8::WriteFlags;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;
use memchr::memmem;

// =================================================================================================
// Base64

/// Marks a character outside both base64 alphabets in [`BASE64_VALUES`].
const INVALID: u8 = 0xff;

/// The value of each character of the standard and URL-safe base64 alphabets, which share all
/// but values 62 (`+` and `-`) and 63 (`/` and `_`), and [`INVALID`] for every other byte. Both
/// of Node.js's decoders accept the two alphabets mixed.
const BASE64_VALUES: [u8; 256] = {
    let mut table = [INVALID; 256];
    let mut value = 0;
    while value < 64 {
        table[STANDARD_ALPHABET[value] as usize] = value as u8;
        value += 1;
    }
    table[b'-' as usize] = 62;
    table[b'_' as usize] = 63;
    table
};

const STANDARD_ALPHABET: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// The size of the buffer [`decode_base64_forgiving`] needs for `src`, as simdutf's
/// `maximal_binary_length_from_base64` computes it: exact for unpadded or padded input without
/// characters to skip, and otherwise an overestimate.
fn maximal_decoded_length(src: &[u8]) -> usize {
    let padding = match src {
        [.., b'=', b'='] => 2,
        [.., b'='] => 1,
        _ => 0,
    };
    let length = src.len() - padding;
    if length % 4 <= 1 {
        length / 4 * 3
    } else {
        length / 4 * 3 + length % 4 - 1
    }
}

/// Decodes `src` into `dst` as simdutf's `base64_to_binary` does with
/// `base64_default_or_url_accept_garbage`, the decoder behind `Buffer.from(string, 'base64')`.
/// Returns the number of bytes written.
///
/// The input ends at its first `=`. Characters of either alphabet are decoded in groups of four
/// and every other character is skipped. A final group of two or three characters decodes to one
/// or two bytes, ignoring the bits left over, and a final group of one character decodes to
/// nothing.
///
/// `dst` must be at least [`maximal_decoded_length`] bytes long.
fn decode_base64_forgiving(src: &[u8], dst: &mut [u8]) -> usize {
    let src = memchr::memchr(b'=', src).map_or(src, |end| &src[..end]);

    // Input in one alphabet, with nothing to skip, a whole number of bytes in its final group
    // and none of its bits left over, which is what an encoder produces.
    if let Ok(decoded) = STANDARD_NO_PAD.decode(src, Out::from_slice(dst)) {
        return decoded.len();
    }
    if let Ok(decoded) = URL_SAFE_NO_PAD.decode(src, Out::from_slice(dst)) {
        return decoded.len();
    }

    // Anything else: keep the characters of either alphabet, as standard ones, and drop what a
    // final group decodes without.
    let mut kept: Vec<u8> = src
        .iter()
        .map(|&c| BASE64_VALUES[usize::from(c)])
        .filter(|&value| value != INVALID)
        .map(|value| STANDARD_ALPHABET[usize::from(value)])
        .collect();
    let last_bits = match kept.len() % 4 {
        1 => {
            kept.pop();
            None
        }
        2 => Some(0x30),
        3 => Some(0x3c),
        _ => None,
    };
    if let (Some(mask), Some(last)) = (last_bits, kept.last_mut()) {
        let value = BASE64_VALUES[usize::from(*last)] & mask;
        *last = STANDARD_ALPHABET[usize::from(value)];
    }
    // `kept` is now valid and canonical, so this cannot fail.
    STANDARD_NO_PAD
        .decode(&kept, Out::from_slice(dst))
        .map_or(0, |decoded| decoded.len())
}

/// Decodes `src` into `dst` as nbytes' `Base64Decode` does, the decoder behind
/// `buffer.write(string, 'base64')`, and returns the number of bytes written. Decoding stops
/// when `dst` is full.
///
/// Groups of four characters of either alphabet decode at once. A group with any other character
/// is decoded a character at a time, skipping characters outside the alphabets, until an `=` or
/// the end of the input.
fn decode_base64_legacy(dst: &mut [u8], src: &[u8]) -> usize {
    let available = dst.len().min(legacy_decoded_size(src));
    let write_end = available / 3 * 3;
    let mut read_end = src.len() / 4 * 4;
    let mut read = 0;
    let mut written = 0;
    while read < read_end && written < write_end {
        let v0 = BASE64_VALUES[usize::from(src[read])];
        let v1 = BASE64_VALUES[usize::from(src[read + 1])];
        let v2 = BASE64_VALUES[usize::from(src[read + 2])];
        let v3 = BASE64_VALUES[usize::from(src[read + 3])];
        if (v0 | v1 | v2 | v3) & 0x80 != 0 {
            if !decode_base64_group_slow(dst, src, &mut read, &mut written) {
                return written;
            }
            read_end = read + (src.len() - read) / 4 * 4;
        } else {
            dst[written] = (v0 << 2) | (v1 >> 4);
            dst[written + 1] = (v1 << 4) | (v2 >> 2);
            dst[written + 2] = (v2 << 6) | v3;
            read += 4;
            written += 3;
        }
    }
    if read < src.len() && written < dst.len() {
        decode_base64_group_slow(dst, src, &mut read, &mut written);
    }
    written
}

/// nbytes' `Base64DecodedSize`: the bytes `src` decodes to if every character but up to two
/// final `=` is in the alphabet.
fn legacy_decoded_size(src: &[u8]) -> usize {
    if src.len() < 2 {
        return 0;
    }
    let mut size = src.len();
    if src[size - 1] == b'=' {
        size -= 1;
        if src[size - 1] == b'=' {
            size -= 1;
        }
    }
    if size > 1 {
        (size / 4) * 3 + (size % 4).div_ceil(2)
    } else {
        0
    }
}

/// nbytes' `Base64DecodeGroupSlow`: decodes one group of four characters from `src[*read..]`
/// into `dst[*written..]`, skipping characters outside the alphabets, and advances `*read` and
/// `*written`. Returns `false` when decoding should stop: at an `=`, at the end of `src`, or when
/// `dst` is full.
///
/// `*read` must be less than `src.len()`.
fn decode_base64_group_slow(
    dst: &mut [u8],
    src: &[u8],
    read: &mut usize,
    written: &mut usize,
) -> bool {
    let mut hi = 0;
    for position in 0..4 {
        let lo = loop {
            let c = src[*read];
            let value = BASE64_VALUES[usize::from(c)];
            *read += 1;
            if value < 64 {
                break value;
            }
            if c == b'=' || *read >= src.len() {
                return false;
            }
        };
        let byte = match position {
            1 => Some(((hi & 0x3f) << 2) | ((lo & 0x30) >> 4)),
            2 => Some(((hi & 0x0f) << 4) | ((lo & 0x3c) >> 2)),
            3 => Some(((hi & 0x03) << 6) | (lo & 0x3f)),
            _ => None,
        };
        if let Some(byte) = byte {
            dst[*written] = byte;
            *written += 1;
        }
        if *read >= src.len() || *written >= dst.len() {
            return false;
        }
        hi = lo;
    }
    true
}

// =================================================================================================
// Search

// Searches follow the strategies of V8's `StringSearch`, which the C++ used. A needle shorter
// than `LONG_NEEDLE` is found by scanning with `memchr` for its first byte or code unit, which
// costs nothing to set up and skips most of a haystack in which that is rare. A longer one of
// bytes goes to `memmem`, and of code units to a scan by its first code unit and then
// Boyer-Moore-Horspool, since `memmem`'s prefilter finds few rare bytes in a needle whose code
// units' high bytes are all zero. A scan by the first byte or code unit hands the rest of the
// haystack on, to `memmem` or Boyer-Moore-Horspool, once the positions it finds come too close
// together for it to skip much (see `sparse`).

/// Needles at least this long, in bytes or code units, are long, as V8's `kBMMinPatternLength`.
const LONG_NEEDLE: usize = 8;

/// How many positions a scan by the first byte or code unit checks however close together they
/// are.
const CANDIDATES: usize = 16;

/// The average distance in bytes between the positions a scan by the first byte or code unit
/// checks, below which a call to `memchr` per position costs more than `memmem` does.
const SPARSE_GAP: usize = 64;

/// Whether a scan by the first byte or code unit goes on, after checking `checked` positions in
/// the first `scanned` bytes of the haystack: while they are on average at least `gap` bytes
/// apart, once the first [`CANDIDATES`] are checked.
const fn sparse(checked: usize, scanned: usize, gap: usize) -> bool {
    checked <= CANDIDATES + scanned / gap
}

/// Finds `needle` in `haystack` as `SearchString` does in the C++: the first match at or after
/// `start` when `is_forward`, and otherwise the last match at or before `start`. Returns the
/// match's position, or `haystack.len()` if there is none.
///
/// `needle` must be non-empty and no longer than `haystack`, and when `is_forward`, `start` must
/// leave room for it.
fn search_one_byte(haystack: &[u8], needle: &[u8], start: usize, is_forward: bool) -> usize {
    let found = if is_forward {
        find_bytes(&haystack[start..], needle).map(|position| position + start)
    } else {
        let last_start = start.min(haystack.len() - needle.len());
        rfind_bytes(&haystack[..last_start + needle.len()], needle)
    };
    found.unwrap_or(haystack.len())
}

/// The first position of `needle` in `haystack`, which is at least as long.
fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.len() >= LONG_NEEDLE {
        return memmem::find(haystack, needle);
    }
    let last = haystack.len() - needle.len();
    let mut from = 0;
    let mut checked = 0;
    loop {
        let at = from + memchr::memchr(needle[0], &haystack[from..=last])?;
        if haystack[at..].starts_with(needle) {
            return Some(at);
        }
        from = at + 1;
        checked += 1;
        if from > last {
            return None;
        }
        if !sparse(checked, from, SPARSE_GAP) {
            return memmem::find(&haystack[from..], needle).map(|position| position + from);
        }
    }
}

/// The last position of `needle` in `haystack`, which is at least as long.
fn rfind_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.len() >= LONG_NEEDLE {
        return memmem::rfind(haystack, needle);
    }
    // The positions left to check are those before `end`.
    let start_end = haystack.len() - needle.len() + 1;
    let mut end = start_end;
    let mut checked = 0;
    loop {
        let at = memchr::memrchr(needle[0], &haystack[..end])?;
        if haystack[at..].starts_with(needle) {
            return Some(at);
        }
        end = at;
        checked += 1;
        if !sparse(checked, start_end - end, SPARSE_GAP) {
            return memmem::rfind(&haystack[..end + needle.len() - 1], needle);
        }
    }
}

/// [`search_one_byte`] over UTF-16 code units, held as native-endian bytes in `haystack` and
/// `needle`, both of even length. `start` and the result are in code units, and the result is
/// `haystack.len() / 2` if there is no match.
fn search_two_byte(haystack: &[u8], needle: &[u8], start: usize, is_forward: bool) -> usize {
    let (haystack, _) = haystack.as_chunks::<2>();
    let (needle, _) = needle.as_chunks::<2>();
    let found = if is_forward {
        find_units(&haystack[start..], needle).map(|position| position + start)
    } else {
        let last_start = start.min(haystack.len() - needle.len());
        rfind_units(&haystack[..last_start + needle.len()], needle)
    };
    found.unwrap_or(haystack.len())
}

/// The byte of `unit` that a search by it scans for, and its index in the unit: the larger byte,
/// which is the nonzero one for most text.
fn scan_byte(unit: [u8; 2]) -> (u8, usize) {
    let lane = usize::from(unit[1] > unit[0]);
    (unit[lane], lane)
}

/// The average distance in bytes between the positions a scan by the first code unit checks,
/// below which it hands the haystack on, for `needle`. Boyer-Moore-Horspool skips more of the
/// haystack at each step the longer the needle is.
fn sparse_gap(needle: &[[u8; 2]]) -> usize {
    if needle.len() >= LONG_NEEDLE {
        SPARSE_GAP.max(needle.len() * 5)
    } else {
        SPARSE_GAP
    }
}

/// The first position of `needle` in `haystack`, which is at least as long.
fn find_units(haystack: &[[u8; 2]], needle: &[[u8; 2]]) -> Option<usize> {
    let (byte, lane) = scan_byte(needle[0]);
    let gap = sparse_gap(needle);
    let bytes = haystack.as_flattened();
    // The bytes at which a match's scanned byte can be.
    let end = (haystack.len() - needle.len()) * 2 + lane + 1;
    let mut from = lane;
    let mut checked = 0;
    loop {
        let at = from + memchr::memchr(byte, &bytes[from..end])?;
        if at % 2 == lane && haystack[at / 2..].starts_with(needle) {
            return Some(at / 2);
        }
        from = at + 1;
        checked += 1;
        if from >= end {
            return None;
        }
        if !sparse(checked, from, gap) {
            break;
        }
    }
    let unit = from / 2;
    let rest = &haystack[unit..];
    let found = if needle.len() >= LONG_NEEDLE {
        find_units_horspool(rest, needle)
    } else {
        search_units_linear(rest, needle, true)
    };
    found.map(|position| position + unit)
}

/// The last position of `needle` in `haystack`, which is at least as long.
fn rfind_units(haystack: &[[u8; 2]], needle: &[[u8; 2]]) -> Option<usize> {
    let (byte, lane) = scan_byte(needle[0]);
    let gap = sparse_gap(needle);
    let bytes = haystack.as_flattened();
    // The bytes left to check are those before `end`.
    let start_end = (haystack.len() - needle.len()) * 2 + lane + 1;
    let mut end = start_end;
    let mut checked = 0;
    loop {
        let at = memchr::memrchr(byte, &bytes[..end])?;
        if at % 2 == lane && haystack[at / 2..].starts_with(needle) {
            return Some(at / 2);
        }
        end = at;
        checked += 1;
        if !sparse(checked, start_end - end, gap) {
            break;
        }
    }
    // The code units left to check are those whose scanned byte is before `end`, which are
    // before `end.div_ceil(2)`. That can include the last one checked, which did not match.
    let units = end.div_ceil(2);
    let rest = &haystack[..units + needle.len() - 1];
    if needle.len() >= LONG_NEEDLE {
        rfind_units_horspool(rest, needle)
    } else {
        search_units_linear(rest, needle, false)
    }
}

/// The key of a code unit in a Horspool shift table: the low byte of its value, as in V8.
fn unit_key(unit: [u8; 2]) -> usize {
    usize::from(u16::from_ne_bytes(unit).to_le_bytes()[0])
}

/// Boyer-Moore-Horspool over code units, which skips most of a haystack of text. A needle that
/// matches the haystack in many places can make it compare most of the needle at most
/// positions, so once it has compared about as many code units as the haystack holds, it
/// finishes with [`search_units_linear`], which takes linear time.
fn find_units_horspool(haystack: &[[u8; 2]], needle: &[[u8; 2]]) -> Option<usize> {
    let (&last, prefix) = needle.split_last()?;
    let mut shift = [needle.len(); 256];
    for (i, &unit) in prefix.iter().enumerate() {
        shift[unit_key(unit)] = prefix.len() - i;
    }
    let mut budget = haystack.len();
    let mut at = 0;
    while at + needle.len() <= haystack.len() {
        let tail = haystack[at + prefix.len()];
        if tail == last {
            if haystack[at..at + prefix.len()] == *prefix {
                return Some(at);
            }
            let Some(rest) = budget.checked_sub(needle.len()) else {
                return search_units_linear(&haystack[at..], needle, true)
                    .map(|position| position + at);
            };
            budget = rest;
        }
        at += shift[unit_key(tail)];
    }
    None
}

/// [`find_units_horspool`] from the end.
fn rfind_units_horspool(haystack: &[[u8; 2]], needle: &[[u8; 2]]) -> Option<usize> {
    let (&first, suffix) = needle.split_first()?;
    let mut shift = [needle.len(); 256];
    for (i, &unit) in suffix.iter().enumerate().rev() {
        shift[unit_key(unit)] = i + 1;
    }
    let mut budget = haystack.len();
    let mut at = haystack.len().checked_sub(needle.len())?;
    loop {
        let head = haystack[at];
        if head == first {
            if haystack[at + 1..at + needle.len()] == *suffix {
                return Some(at);
            }
            let Some(rest) = budget.checked_sub(needle.len()) else {
                return search_units_linear(&haystack[..at + needle.len()], needle, false);
            };
            budget = rest;
        }
        at = at.checked_sub(shift[unit_key(head)])?;
    }
}

/// Finds the first or last match of a non-empty `needle` with Knuth-Morris-Pratt search over
/// code units. The prefix table preserves partial matches without examining odd byte offsets,
/// so repeated byte matches at those offsets cannot increase the work beyond linear time.
fn search_units_linear(
    haystack: &[[u8; 2]],
    needle: &[[u8; 2]],
    is_forward: bool,
) -> Option<usize> {
    let mut prefix = vec![0; needle.len()];
    let mut matched = 0;
    for i in 1..needle.len() {
        while matched > 0 && needle[i] != needle[matched] {
            matched = prefix[matched - 1];
        }
        if needle[i] == needle[matched] {
            matched += 1;
        }
        prefix[i] = matched;
    }

    let mut found = None;
    matched = 0;
    for (i, unit) in haystack.iter().enumerate() {
        while matched > 0 && *unit != needle[matched] {
            matched = prefix[matched - 1];
        }
        if *unit == needle[matched] {
            matched += 1;
        }
        if matched == needle.len() {
            let position = i + 1 - needle.len();
            if is_forward {
                return Some(position);
            }
            found = Some(position);
            matched = prefix[matched - 1];
        }
    }
    found
}

// =================================================================================================
// Helpers

/// Lowercase hex digits for each byte value.
const HEX_PAIRS: [[u8; 2]; 256] = {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut table = [[0; 2]; 256];
    let mut byte = 0;
    while byte < 256 {
        table[byte] = [DIGITS[byte >> 4], DIGITS[byte & 0xf]];
        byte += 1;
    }
    table
};

/// Stack space for short results, as the C++ used `KJ_STACK_ARRAY`.
const STACK_BUFFER_SIZE: usize = 1024;

/// Calls `f` with `len` bytes of uninitialized scratch space, on the stack when it fits.
fn with_scratch<R>(len: usize, f: impl FnOnce(&mut [MaybeUninit<u8>]) -> R) -> R {
    let mut stack = [MaybeUninit::<u8>::uninit(); STACK_BUFFER_SIZE];
    if len <= stack.len() {
        f(&mut stack[..len])
    } else {
        let mut heap = Vec::with_capacity(len);
        f(&mut heap.spare_capacity_mut()[..len])
    }
}

/// Calls `f` with `len` zeroed bytes of scratch space, on the stack when it fits.
fn with_zeroed_scratch<R>(len: usize, f: impl FnOnce(&mut [u8]) -> R) -> R {
    let mut stack = [0u8; STACK_BUFFER_SIZE];
    if len <= stack.len() {
        f(&mut stack[..len])
    } else {
        f(&mut vec![0u8; len])
    }
}

/// The bytes of `array`.
///
/// # Safety
///
/// While the slice is live, no JavaScript may run, `array`'s buffer must not be detached or
/// resized, and no mutable slice may overlap it. The methods below hold it only across V8 calls
/// that run no JavaScript: writing or creating strings and allocating buffers. Those can collect
/// garbage, which neither moves nor frees a backing store that a live handle refers to.
unsafe fn bytes(array: v8::Local<'_, v8::Uint8Array>) -> &[u8] {
    // SAFETY: the bytes are `array`'s, and the caller upholds the rest.
    unsafe { &*array.as_raw_slice() }
}

/// The bytes of `array`, mutably.
///
/// # Safety
///
/// As for [`bytes`], and no other slice may overlap this one.
unsafe fn bytes_mut(array: v8::Local<'_, v8::Uint8Array>) -> &mut [u8] {
    // SAFETY: as in `bytes`.
    unsafe { &mut *array.as_raw_slice() }
}

/// The addresses of `bytes`.
fn address_range(bytes: *mut [u8]) -> Range<usize> {
    let start = bytes.cast::<u8>() as usize;
    start..start + bytes.len()
}

/// The `[start, end)` range of an array of `len` bytes, or a `RangeError` if it is out of bounds.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the range is checked to be within the array, so each bound is a non-negative \
              number no greater than its length; a fraction truncates, as in the C++"
)]
fn range(len: usize, start: f64, end: f64) -> jsg::Result<Range<usize>> {
    if 0.0 <= start && start <= end && end <= to_f64(len) {
        Ok(start as usize..end as usize)
    } else {
        Err(jsg::Error::new_range_error("Range is out of bounds"))
    }
}

/// The start index of a search, checking the arguments that [`search_one_byte`] relies on.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the start is checked to be non-negative; a fraction truncates, as in the C++"
)]
fn search_start(
    haystack_len: usize,
    needle_len: usize,
    start_index: f64,
    is_forward: bool,
) -> jsg::Result<usize> {
    if needle_len > 0
        && needle_len <= haystack_len
        && start_index >= 0.0
        && (!is_forward || start_index <= to_f64(haystack_len - needle_len))
    {
        Ok(start_index as usize)
    } else {
        Err(jsg::Error::new_range_error("Invalid search arguments"))
    }
}

#[expect(
    clippy::cast_precision_loss,
    reason = "byte lengths are below 2^53, so they are exact"
)]
fn to_f64(len: usize) -> f64 {
    len as f64
}

/// Only these flags are accepted. Every write passes V8 the capacity of an exact range, so a
/// null terminator would be written past it.
fn write_flags(flags: u8) -> jsg::Result<WriteFlags> {
    match flags {
        0 => Ok(WriteFlags::None),
        2 => Ok(WriteFlags::ReplaceInvalidUtf8),
        _ => Err(jsg::Error::new_type_error("Invalid write flags")),
    }
}

/// The length of `string` in UTF-16 code units.
fn string_length(string: JsString<'_>) -> u32 {
    u32::try_from(string.length()).unwrap_or(0)
}

/// `count` code units of a string of `length`, which `count` does not exceed, as `u32`.
fn units(count: usize, length: u32) -> u32 {
    u32::try_from(count).unwrap_or(length)
}

fn new_one_byte_string<'s>(lock: &mut jsg::Lock<'s>, bytes: &[u8]) -> jsg::Result<JsString<'s>> {
    v8::String::new_from_one_byte(lock, bytes)
        .map(JsString::from)
        .ok_or_else(jsg::Error::invalid_string_length)
}

fn transcode_encoding(encoding: u8) -> jsg::Result<i18n::Encoding> {
    match encoding {
        0 => Ok(i18n::Encoding::Ascii),
        1 => Ok(i18n::Encoding::Latin1),
        2 => Ok(i18n::Encoding::Utf8),
        3 => Ok(i18n::Encoding::Utf16Le),
        _ => Err(jsg::Error::new_error(
            "Unable to transcode buffer due to unsupported encoding",
        )),
    }
}

// =================================================================================================
// Module

#[jsg_resource]
pub struct BufferNative;

#[jsg_resource]
impl BufferNative {
    #[jsg_static_constant]
    pub const WRITE_NONE: u32 = WriteFlags::None as u32;

    #[jsg_static_constant]
    pub const WRITE_REPLACE_INVALID_UTF8: u32 = WriteFlags::ReplaceInvalidUtf8 as u32;

    #[must_use]
    pub fn new(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Self> {
        jsg::Rc::new(lock, Self)
    }

    // V8 strings

    /// The length of `string` in UTF-8.
    #[jsg_method]
    pub fn utf8_length<'s>(&self, lock: &mut jsg::Lock<'s>, string: JsString<'s>) -> f64 {
        to_f64(string.utf8_length(lock))
    }

    /// Writes the low byte of each UTF-16 code unit of `string` into the range. Returns the
    /// number of bytes written.
    #[jsg_method]
    pub fn write_one_byte<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        string: JsString<'s>,
        dest: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
        flags: u8,
    ) -> jsg::Result<f64> {
        let flags = write_flags(flags)?;
        // SAFETY: writing a string runs no JavaScript, and `string` is not in an ArrayBuffer.
        let dest = unsafe { bytes_mut(dest) };
        let range = range(dest.len(), start, end)?;
        let dest = &mut dest[range];
        if dest.is_empty() {
            return Ok(0.0);
        }
        let length = string_length(string);
        let written = dest.len().min(length as usize);
        string.write_one_byte(lock, 0, units(written, length), dest, flags);
        Ok(to_f64(written))
    }

    /// Writes `string` into the range as UTF-8. Returns the number of bytes written.
    #[jsg_method]
    pub fn write_utf8<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        string: JsString<'s>,
        dest: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
        flags: u8,
    ) -> jsg::Result<f64> {
        let flags = write_flags(flags)?;
        // SAFETY: as in `write_one_byte`.
        let dest = unsafe { bytes_mut(dest) };
        let range = range(dest.len(), start, end)?;
        let dest = &mut dest[range];
        if dest.is_empty() {
            return Ok(0.0);
        }
        Ok(to_f64(string.write_utf8(lock, dest, flags)))
    }

    /// Writes `string` into the range as native-endian UTF-16 code units (the last byte of an
    /// odd-length range is not written). Returns the number of code units written.
    #[jsg_method]
    pub fn write_utf16<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        string: JsString<'s>,
        dest: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
        flags: u8,
    ) -> jsg::Result<f64> {
        let flags = write_flags(flags)?;
        // SAFETY: as in `write_one_byte`.
        let dest = unsafe { bytes_mut(dest) };
        let range = range(dest.len(), start, end)?;
        let dest = &mut dest[range];
        let length = string_length(string);
        let written = (dest.len() / 2).min(length as usize);
        if written == 0 {
            return Ok(0.0);
        }
        // SAFETY: every bit pattern is a valid `u16`.
        let (prefix, aligned, _) = unsafe { dest.align_to_mut::<u16>() };
        if prefix.is_empty() {
            string.write(
                lock,
                0,
                units(written, length),
                &mut aligned[..written],
                flags,
            );
        } else {
            let mut code_units = vec![0u16; written];
            string.write(lock, 0, units(written, length), &mut code_units, flags);
            for (pair, unit) in dest.chunks_exact_mut(2).zip(&code_units) {
                pair.copy_from_slice(&unit.to_ne_bytes());
            }
        }
        Ok(to_f64(written))
    }

    /// A string of the range's bytes as Latin-1.
    #[jsg_method]
    pub fn new_from_one_byte<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        // SAFETY: creating a string runs no JavaScript.
        let data = unsafe { self::bytes(bytes) };
        let range = range(data.len(), start, end)?;
        let data = &data[range];
        new_one_byte_string(lock, data)
    }

    /// A string of the range's bytes as UTF-8, replacing invalid sequences.
    #[jsg_method]
    pub fn new_from_utf8<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        // SAFETY: creating a string runs no JavaScript.
        let data = unsafe { self::bytes(bytes) };
        let range = range(data.len(), start, end)?;
        let data = &data[range];
        v8::String::new_from_utf8(lock, data)
            .map(JsString::from)
            .ok_or_else(jsg::Error::invalid_string_length)
    }

    /// A string of the range's bytes as native-endian UTF-16 code units (a trailing odd byte is
    /// ignored).
    #[jsg_method]
    pub fn new_from_two_byte<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        // SAFETY: creating a string runs no JavaScript.
        let data = unsafe { self::bytes(bytes) };
        let range = range(data.len(), start, end)?;
        let data = &data[range];
        let length = data.len() / 2;
        // SAFETY: every bit pattern is a valid `u16`.
        let (prefix, aligned, _) = unsafe { data.align_to::<u16>() };
        let string = if prefix.is_empty() {
            v8::String::new_from_two_byte(lock, &aligned[..length])
        } else {
            let code_units: Vec<u16> = data
                .chunks_exact(2)
                .map(|pair| u16::from_ne_bytes([pair[0], pair[1]]))
                .collect();
            v8::String::new_from_two_byte(lock, &code_units)
        };
        string
            .map(JsString::from)
            .ok_or_else(jsg::Error::invalid_string_length)
    }

    // Search

    /// The position of `needle` in `haystack` (see [`search_one_byte`]), or the haystack's
    /// length if there is none.
    #[jsg_method]
    pub fn search_one_byte<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        haystack: v8::Local<'s, v8::Uint8Array>,
        needle: v8::Local<'s, v8::Uint8Array>,
        start_index: f64,
        is_forward: bool,
    ) -> jsg::Result<f64> {
        let haystack = haystack.as_slice(lock);
        let needle = needle.as_slice(lock);
        let start = search_start(haystack.len(), needle.len(), start_index, is_forward)?;
        Ok(to_f64(search_one_byte(haystack, needle, start, is_forward)))
    }

    /// The position, in UTF-16 code units, of the first `needle_length` code units of `needle`
    /// in the first `haystack_length` code units of `haystack` (see [`search_two_byte`]), or
    /// `haystack_length` if there is none.
    #[jsg_method]
    #[expect(
        clippy::too_many_arguments,
        reason = "the arguments of the JavaScript method, which TypeScript passes positionally"
    )]
    pub fn search_two_byte<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        haystack: v8::Local<'s, v8::Uint8Array>,
        haystack_length: f64,
        needle: v8::Local<'s, v8::Uint8Array>,
        needle_length: f64,
        start_index: f64,
        is_forward: bool,
    ) -> jsg::Result<f64> {
        let haystack = haystack.as_slice(lock);
        let needle = needle.as_slice(lock);
        let haystack = &haystack[..range(haystack.len(), 0.0, haystack_length * 2.0)?.end / 2 * 2];
        let needle = &needle[..range(needle.len(), 0.0, needle_length * 2.0)?.end / 2 * 2];
        let start = search_start(
            haystack.len() / 2,
            needle.len() / 2,
            start_index,
            is_forward,
        )?;
        Ok(to_f64(search_two_byte(haystack, needle, start, is_forward)))
    }

    // Comparison

    /// Compares `length` bytes of each array, from the given starts, returning a negative
    /// number, zero, or a positive number.
    #[jsg_method]
    pub fn memcmp<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        one: v8::Local<'s, v8::Uint8Array>,
        one_start: f64,
        two: v8::Local<'s, v8::Uint8Array>,
        two_start: f64,
        length: f64,
    ) -> jsg::Result<i32> {
        let one = one.as_slice(lock);
        let two = two.as_slice(lock);
        let one = &one[range(one.len(), one_start, one_start + length)?];
        let two = &two[range(two.len(), two_start, two_start + length)?];
        Ok(one.cmp(two) as i32)
    }

    // Base64

    /// Decodes the low bytes of the code units of `string` as base64 into a new array (see
    /// [`decode_base64_forgiving`]).
    #[jsg_method]
    pub fn decode_base64<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        string: JsString<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Uint8Array>> {
        let length = string_length(string);
        with_zeroed_scratch(length as usize, |text| {
            if !text.is_empty() {
                string.write_one_byte(lock, 0, length, text, WriteFlags::ReplaceInvalidUtf8);
            }
            let (buffer, written) =
                v8::ArrayBuffer::new_zeroed_with(lock, maximal_decoded_length(text), |dest| {
                    decode_base64_forgiving(text, dest)
                })
                .ok_or_else(|| {
                    jsg::Error::new_range_error("Failed to allocate memory for Uint8Array")
                })?;
            v8::Uint8Array::from_buffer(lock, &buffer, 0, written)
        })
    }

    /// The range of `bytes` encoded as base64, with padding.
    #[jsg_method]
    pub fn encode_base64<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        let range = range(bytes.len(), start, end)?;
        with_scratch(STANDARD.encoded_length(range.len()), |out| {
            let data = bytes.as_slice(lock);
            let encoded = STANDARD.encode(&data[range], Out::from_uninit_slice(out));
            new_one_byte_string(lock, encoded)
        })
    }

    /// The range of `bytes` encoded as base64url, without padding.
    #[jsg_method]
    pub fn encode_base64_url<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        let range = range(bytes.len(), start, end)?;
        with_scratch(URL_SAFE_NO_PAD.encoded_length(range.len()), |out| {
            let data = bytes.as_slice(lock);
            let encoded = URL_SAFE_NO_PAD.encode(&data[range], Out::from_uninit_slice(out));
            new_one_byte_string(lock, encoded)
        })
    }

    /// Decodes `input` as base64 into the range of `output` (see [`decode_base64_legacy`]).
    /// Returns the number of bytes written.
    #[jsg_method]
    pub fn decode_base64_into<'s>(
        &self,
        output: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
        input: v8::Local<'s, v8::Uint8Array>,
    ) -> jsg::Result<f64> {
        let output = output.as_raw_slice();
        let input = input.as_raw_slice();
        let range = range(output.len(), start, end)?;
        let output_addresses = address_range(output);
        let input_addresses = address_range(input);
        let overlapping = output_addresses.start < input_addresses.end
            && input_addresses.start < output_addresses.end;
        // SAFETY: both are the bytes of arrays, decoding runs no JavaScript, and an `input` that
        // overlaps `output` is copied out before `output` is borrowed.
        let written = unsafe {
            if overlapping {
                let input = (*input).to_vec();
                decode_base64_legacy(&mut (&mut *output)[range], &input)
            } else {
                decode_base64_legacy(&mut (&mut *output)[range], &*input)
            }
        };
        Ok(to_f64(written))
    }

    // Hex

    /// The range of `bytes` encoded as lowercase hex.
    #[jsg_method]
    pub fn encode_hex<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
        start: f64,
        end: f64,
    ) -> jsg::Result<JsString<'s>> {
        let range = range(bytes.len(), start, end)?;
        with_zeroed_scratch(range.len() * 2, |out| {
            let data = bytes.as_slice(lock);
            for (digits, &byte) in out.chunks_exact_mut(2).zip(&data[range]) {
                digits.copy_from_slice(&HEX_PAIRS[usize::from(byte)]);
            }
            new_one_byte_string(lock, out)
        })
    }

    // Validation

    /// Whether every byte of `input` is ASCII.
    #[jsg_method]
    pub fn validate_ascii<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: v8::Local<'s, v8::Uint8Array>,
    ) -> bool {
        input.as_slice(lock).is_ascii()
    }

    /// Whether `input` is valid UTF-8.
    #[jsg_method]
    pub fn validate_utf8<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: v8::Local<'s, v8::Uint8Array>,
    ) -> bool {
        simdutf8::basic::from_utf8(input.as_slice(lock)).is_ok()
    }

    // Byte order

    /// Reverses the byte order of each 16-bit unit of `data`. Returns `false`, changing
    /// nothing, if its length is not a multiple of 2.
    #[jsg_method]
    pub fn swap_bytes16(&self, data: v8::Local<'_, v8::Uint8Array>) -> bool {
        // SAFETY: nothing else runs while the slice is live.
        let (units, rest) = unsafe { bytes_mut(data) }.as_chunks_mut::<2>();
        if !rest.is_empty() {
            return false;
        }
        for unit in units {
            *unit = u16::from_ne_bytes(*unit).swap_bytes().to_ne_bytes();
        }
        true
    }

    /// Reverses the byte order of each 32-bit unit of `data`. Returns `false`, changing
    /// nothing, if its length is not a multiple of 4.
    #[jsg_method]
    pub fn swap_bytes32(&self, data: v8::Local<'_, v8::Uint8Array>) -> bool {
        // SAFETY: nothing else runs while the slice is live.
        let (units, rest) = unsafe { bytes_mut(data) }.as_chunks_mut::<4>();
        if !rest.is_empty() {
            return false;
        }
        for unit in units {
            *unit = u32::from_ne_bytes(*unit).swap_bytes().to_ne_bytes();
        }
        true
    }

    /// Reverses the byte order of each 64-bit unit of `data`. Returns `false`, changing
    /// nothing, if its length is not a multiple of 8.
    #[jsg_method]
    pub fn swap_bytes64(&self, data: v8::Local<'_, v8::Uint8Array>) -> bool {
        // SAFETY: nothing else runs while the slice is live.
        let (units, rest) = unsafe { bytes_mut(data) }.as_chunks_mut::<8>();
        if !rest.is_empty() {
            return false;
        }
        for unit in units {
            *unit = u64::from_ne_bytes(*unit).swap_bytes().to_ne_bytes();
        }
        true
    }

    // Transcoding

    /// `source` transcoded between two of the encodings ASCII (0), Latin-1 (1), UTF-8 (2) and
    /// UTF-16LE (3), into a new array.
    #[jsg_method]
    pub fn transcode<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        source: v8::Local<'s, v8::Uint8Array>,
        from_encoding: u8,
        to_encoding: u8,
    ) -> jsg::Result<v8::Local<'s, v8::Uint8Array>> {
        let from = transcode_encoding(from_encoding)?;
        let to = transcode_encoding(to_encoding)?;
        // SAFETY: transcoding allocates its result and runs no JavaScript.
        let source = unsafe { bytes(source) };
        i18n::transcode(lock, source, from, to)
    }
}

#[cfg(test)]
#[path = "buffer-test.rs"]
mod tests;
