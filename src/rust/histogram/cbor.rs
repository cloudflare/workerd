// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Node.js's histogram export format (`histogram.export()` and `perf_hooks.importHistogram()`):
//! a CBOR (RFC 8949) map with integer keys.
//!
//! ```text
//!   0  -> uint    format version (2)
//!   1  -> uint    lowest discernible value
//!   2  -> uint    highest trackable value
//!   3  -> uint    significant figures
//!   4  -> uint    total count
//!   5  -> uint    min value (the raw field: smallest nonzero value, or i64::MAX)
//!   6  -> uint    max value (the raw field)
//!   7  -> uint    normalizing index offset
//!   8  -> float64 conversion ratio
//!   9  -> uint    counts array length
//!   10 -> array   sparse counts as flat [delta, count, ...]; the first delta is the absolute
//!                 index, and each later one the distance from the previous index
//!   11 -> map     EWMA state, omitted when the EWMA is disabled:
//!        0 -> float64 alpha
//!        1 -> float64 mean
//!        2 -> float64 variance
//!        3 -> float64 error rate
//!        4 -> uint    threshold
//! ```
//!
//! Import accepts versions 1 and 2, whose layouts are identical. Version 2 data may contain keys
//! the importer does not know, which are skipped; version 1 data may not. Any field may be
//! absent; the total count, min, and max are then derived from the counts.
//!
//! The one difference from Node.js is that import rejects a nonzero normalizing index offset.
//! Node.js never writes one, and [`crate::hdr`] does not port the index normalization it implies.

use crate::Error;
use crate::Histogram;
use crate::Options;

const MAJOR_UINT: u8 = 0 << 5;
const MAJOR_ARRAY: u8 = 4 << 5;
const MAJOR_MAP: u8 = 5 << 5;
const FLOAT64: u8 = 0xfb;

// Additional information values for arguments that follow the initial byte in 1, 2, 4, or 8 bytes.
const INFO_U8: u8 = 0x18;
const INFO_U16: u8 = 0x19;
const INFO_U32: u8 = 0x1a;
const INFO_U64: u8 = 0x1b;

const EXPORT_VERSION: u64 = 2;
const STRICT_EXPORT_VERSION: u64 = 1;

const KEY_VERSION: u64 = 0;
const KEY_LOWEST: u64 = 1;
const KEY_HIGHEST: u64 = 2;
const KEY_FIGURES: u64 = 3;
const KEY_TOTAL_COUNT: u64 = 4;
const KEY_MIN: u64 = 5;
const KEY_MAX: u64 = 6;
const KEY_NORM_OFFSET: u64 = 7;
const KEY_CONV_RATIO: u64 = 8;
const KEY_COUNTS_LEN: u64 = 9;
const KEY_COUNTS: u64 = 10;
const KEY_EWMA: u64 = 11;

const EWMA_ALPHA: u64 = 0;
const EWMA_MEAN: u64 = 1;
const EWMA_VARIANCE: u64 = 2;
const EWMA_ERROR_RATE: u64 = 3;
const EWMA_THRESHOLD: u64 = 4;

/// The maximum nesting depth of the unknown values that import skips over.
const MAX_SKIP_DEPTH: u32 = 16;

fn write_uint(out: &mut Vec<u8>, major: u8, value: u64) {
    let bytes = value.to_be_bytes();
    match value {
        0..=23 => out.push(major | bytes[7]),
        24..=0xff => out.extend_from_slice(&[major | INFO_U8, bytes[7]]),
        0x100..=0xffff => {
            out.push(major | INFO_U16);
            out.extend_from_slice(&bytes[6..]);
        }
        0x1_0000..=0xffff_ffff => {
            out.push(major | INFO_U32);
            out.extend_from_slice(&bytes[4..]);
        }
        _ => {
            out.push(major | INFO_U64);
            out.extend_from_slice(&bytes);
        }
    }
}

fn write_float64(out: &mut Vec<u8>, value: f64) {
    out.push(FLOAT64);
    out.extend_from_slice(&value.to_bits().to_be_bytes());
}

/// A cursor over CBOR input. Every read returns `None` on malformed or truncated input.
struct Reader<'a> {
    data: &'a [u8],
}

impl Reader<'_> {
    fn remaining(&self) -> usize {
        self.data.len()
    }

    fn peek(&self) -> Option<u8> {
        self.data.first().copied()
    }

    fn take(&mut self, n: usize) -> Option<&[u8]> {
        if n > self.data.len() {
            return None;
        }
        let (head, tail) = self.data.split_at(n);
        self.data = tail;
        Some(head)
    }

    fn take_be(&mut self, n: usize) -> Option<u64> {
        Some(
            self.take(n)?
                .iter()
                .fold(0_u64, |acc, &byte| (acc << 8) | u64::from(byte)),
        )
    }

    /// Reads the argument of a data item of any major type.
    fn read_argument_any(&mut self) -> Option<u64> {
        let info = self.take(1)?[0] & 0x1f;
        match info {
            0..=23 => Some(u64::from(info)),
            24 => self.take_be(1),
            25 => self.take_be(2),
            26 => self.take_be(4),
            27 => self.take_be(8),
            // Indefinite lengths and reserved values are not supported.
            _ => None,
        }
    }

    /// Reads the argument of a data item that must have the given major type.
    fn read_argument(&mut self, major: u8) -> Option<u64> {
        if self.peek()? & 0xe0 != major {
            return None;
        }
        self.read_argument_any()
    }

    /// Reads an unsigned integer that must fit into a non-negative i64.
    fn read_i64(&mut self) -> Option<i64> {
        i64::try_from(self.read_argument(MAJOR_UINT)?).ok()
    }

    /// Reads an unsigned integer that must fit into a non-negative i32.
    fn read_i32(&mut self) -> Option<i32> {
        i32::try_from(self.read_argument(MAJOR_UINT)?).ok()
    }

    /// Reads a float64, or an unsigned integer converted to f64.
    fn read_number(&mut self) -> Option<f64> {
        if self.peek()? == FLOAT64 {
            self.take(1)?;
            return Some(f64::from_bits(self.take_be(8)?));
        }
        #[expect(
            clippy::cast_precision_loss,
            reason = "Node.js converts the integer to double the same way"
        )]
        Some(self.read_argument(MAJOR_UINT)? as f64)
    }

    /// Skips one well-formed data item, including any items nested within it.
    fn skip_item(&mut self, depth: u32) -> Option<()> {
        if depth > MAX_SKIP_DEPTH {
            return None;
        }
        let initial = self.peek()?;
        let major = initial >> 5;
        let info = initial & 0x1f;

        if major == 7 {
            // Simple values and floats: 24 to 27 are followed by 1, 2, 4, or 8 bytes; 28 to 30
            // are reserved, and 31 is a break.
            let extra = match info {
                0..=23 => 0,
                24..=27 => 1_usize << (info - 24),
                _ => return None,
            };
            self.take(1 + extra)?;
            return Some(());
        }

        let arg = self.read_argument_any()?;
        match major {
            // Unsigned and negative integers.
            0 | 1 => Some(()),
            // Byte and text strings.
            2 | 3 => {
                self.take(usize::try_from(arg).ok()?)?;
                Some(())
            }
            // Arrays and maps. Each item takes at least one byte, which also bounds the loop.
            4 | 5 => {
                if arg > self.remaining() as u64 {
                    return None;
                }
                let items = if major == 4 { arg } else { arg * 2 };
                for _ in 0..items {
                    self.skip_item(depth + 1)?;
                }
                Some(())
            }
            // Tags.
            6 => self.skip_item(depth + 1),
            _ => None,
        }
    }
}

/// Records which keys of a map have been read, to reject duplicates. All known keys are below 64.
#[derive(Default)]
struct SeenKeys(u64);

impl SeenKeys {
    fn mark(&mut self, key: u64) -> Option<()> {
        let bit = 1_u64 << key;
        if self.0 & bit != 0 {
            return None;
        }
        self.0 |= bit;
        Some(())
    }

    fn has(&self, key: u64) -> bool {
        self.0 & (1_u64 << key) != 0
    }
}

impl Histogram {
    /// Serializes the histogram in Node.js's export format, version 2.
    pub fn export(&self) -> Vec<u8> {
        let hdr = &self.hdr;
        let non_zero = hdr.counts().iter().filter(|&&count| count != 0).count();
        let has_ewma = self.ewma_alpha > 0.0;

        let mut out = Vec::with_capacity(64 + non_zero * 10);
        write_uint(&mut out, MAJOR_MAP, if has_ewma { 12 } else { 11 });

        let fields = [
            (KEY_VERSION, EXPORT_VERSION),
            (KEY_LOWEST, hdr.lowest_discernible_value().cast_unsigned()),
            (KEY_HIGHEST, hdr.highest_trackable_value().cast_unsigned()),
            (
                KEY_FIGURES,
                u64::from(hdr.significant_figures().cast_unsigned()),
            ),
            (KEY_TOTAL_COUNT, hdr.total_count().cast_unsigned()),
            (KEY_MIN, hdr.min_value().cast_unsigned()),
            (KEY_MAX, hdr.max_value().cast_unsigned()),
            (KEY_NORM_OFFSET, 0),
        ];
        for (key, value) in fields {
            write_uint(&mut out, MAJOR_UINT, key);
            write_uint(&mut out, MAJOR_UINT, value);
        }
        write_uint(&mut out, MAJOR_UINT, KEY_CONV_RATIO);
        write_float64(&mut out, hdr.conversion_ratio());
        write_uint(&mut out, MAJOR_UINT, KEY_COUNTS_LEN);
        write_uint(
            &mut out,
            MAJOR_UINT,
            u64::from(hdr.counts_len().cast_unsigned()),
        );

        write_uint(&mut out, MAJOR_UINT, KEY_COUNTS);
        write_uint(&mut out, MAJOR_ARRAY, non_zero as u64 * 2);
        let mut prev_index = 0_u64;
        for (index, &count) in (0_u64..).zip(hdr.counts()) {
            if count != 0 {
                write_uint(&mut out, MAJOR_UINT, index - prev_index);
                write_uint(&mut out, MAJOR_UINT, count.cast_unsigned());
                prev_index = index;
            }
        }

        if has_ewma {
            write_uint(&mut out, MAJOR_UINT, KEY_EWMA);
            write_uint(&mut out, MAJOR_MAP, 5);
            for (key, value) in [
                (EWMA_ALPHA, self.ewma_alpha),
                (EWMA_MEAN, self.ewma_mean),
                (EWMA_VARIANCE, self.ewma_variance),
                (EWMA_ERROR_RATE, self.ewma_error_rate),
            ] {
                write_uint(&mut out, MAJOR_UINT, key);
                write_float64(&mut out, value);
            }
            write_uint(&mut out, MAJOR_UINT, EWMA_THRESHOLD);
            write_uint(&mut out, MAJOR_UINT, self.threshold.cast_unsigned());
        }
        out
    }

    /// Deserializes a histogram from Node.js's export format, versions 1 and 2.
    pub fn import(data: &[u8]) -> Result<Self, Error> {
        import(data).ok_or(Error::InvalidExportData)
    }
}

#[expect(
    clippy::too_many_lines,
    reason = "a single pass over the map, as in Node.js"
)]
fn import(data: &[u8]) -> Option<Histogram> {
    let mut r = Reader { data };
    let map_size = r.read_argument(MAJOR_MAP)?;

    let mut lowest = 1_i64;
    let mut highest = i64::MAX;
    let mut figures = 3_i32;
    let mut total_count = 0_i64;
    let mut min_value = i64::MAX;
    let mut max_value = 0_i64;
    let mut norm_offset = 0_i32;
    let mut conv_ratio = 1.0_f64;
    let mut counts_len = 0_i32;
    // Data without a version key is imported as version 1.
    let mut version = STRICT_EXPORT_VERSION;

    let mut seen_keys = SeenKeys::default();
    let mut seen_ewma_keys = SeenKeys::default();
    // Unknown keys are skipped while parsing, because the version key that determines whether they
    // are allowed can appear anywhere in the map.
    let mut has_unknown_keys = false;

    let mut sparse_counts: Vec<(i32, i64)> = Vec::new();

    let mut ewma_alpha = 0.0_f64;
    let mut ewma_mean = 0.0_f64;
    let mut ewma_variance = 0.0_f64;
    let mut ewma_error_rate = 0.0_f64;
    let mut threshold = 0_i64;

    for _ in 0..map_size {
        let key = r.read_argument(MAJOR_UINT)?;
        if key > KEY_EWMA {
            r.skip_item(0)?;
            has_unknown_keys = true;
            continue;
        }
        seen_keys.mark(key)?;

        match key {
            KEY_VERSION => {
                version = r.read_argument(MAJOR_UINT)?;
                if !(STRICT_EXPORT_VERSION..=EXPORT_VERSION).contains(&version) {
                    return None;
                }
            }
            KEY_LOWEST => lowest = r.read_i64()?,
            KEY_HIGHEST => highest = r.read_i64()?,
            KEY_FIGURES => figures = r.read_i32()?,
            KEY_TOTAL_COUNT => total_count = r.read_i64()?,
            KEY_MIN => min_value = r.read_i64()?,
            KEY_MAX => max_value = r.read_i64()?,
            KEY_NORM_OFFSET => norm_offset = r.read_i32()?,
            KEY_CONV_RATIO => conv_ratio = r.read_number()?,
            KEY_COUNTS_LEN => counts_len = r.read_i32()?,
            KEY_COUNTS => {
                let arr_len = r.read_argument(MAJOR_ARRAY)?;
                if arr_len % 2 != 0 {
                    return None;
                }
                // Each element takes at least one byte, so a length beyond the remaining input
                // is invalid; checking first avoids reserving memory for it.
                if arr_len > r.remaining() as u64 {
                    return None;
                }
                sparse_counts.reserve(usize::try_from(arr_len / 2).ok()?);
                let mut index = 0_i64;
                for j in (0..arr_len).step_by(2) {
                    let delta = r.read_i32()?;
                    let count = r.read_i64()?;
                    // Indexes are strictly increasing, so only the first delta may be zero.
                    if j > 0 && delta == 0 {
                        return None;
                    }
                    index += i64::from(delta);
                    sparse_counts.push((i32::try_from(index).ok()?, count));
                }
            }
            KEY_EWMA => {
                let sub_size = r.read_argument(MAJOR_MAP)?;
                for _ in 0..sub_size {
                    let sub_key = r.read_argument(MAJOR_UINT)?;
                    if sub_key > EWMA_THRESHOLD {
                        r.skip_item(0)?;
                        has_unknown_keys = true;
                        continue;
                    }
                    seen_ewma_keys.mark(sub_key)?;
                    match sub_key {
                        EWMA_ALPHA => ewma_alpha = r.read_number()?,
                        EWMA_MEAN => ewma_mean = r.read_number()?,
                        EWMA_VARIANCE => ewma_variance = r.read_number()?,
                        EWMA_ERROR_RATE => ewma_error_rate = r.read_number()?,
                        _ => threshold = r.read_i64()?,
                    }
                }
            }
            _ => unreachable!("keys above KEY_EWMA are skipped"),
        }
    }

    // Version 1 data keeps its original semantics: unknown keys are rejected.
    if version == STRICT_EXPORT_VERSION && has_unknown_keys {
        return None;
    }

    // alpha = 1 - 2^(-1/halfLife), so halfLife = -1 / log2(1 - alpha). Like Node.js, the
    // histogram recomputes alpha from the half-life rather than keeping the imported value.
    let half_life = if ewma_alpha > 0.0 && ewma_alpha < 1.0 {
        -1.0 / (1.0 - ewma_alpha).log2()
    } else {
        0.0
    };
    let mut histogram = Histogram::new(&Options {
        lowest,
        highest,
        figures,
        half_life,
        threshold,
    })
    .ok()?;

    if histogram.hdr.counts_len() != counts_len {
        return None;
    }
    if norm_offset != 0 {
        return None;
    }

    let mut observed_total_count = 0_i64;
    {
        let counts = histogram.hdr.counts_mut();
        for (index, count) in sparse_counts {
            let slot = counts.get_mut(usize::try_from(index).ok()?)?;
            // The counts must add up without overflowing i64.
            observed_total_count = observed_total_count.checked_add(count)?;
            *slot = count;
        }
    }
    histogram.hdr.set_conversion_ratio(conv_ratio);

    // Derive the total count, min, and max from the counts. A total count that is present must
    // match them; min and max values that are present are restored as recorded.
    histogram.hdr.reset_internal_counters();
    if seen_keys.has(KEY_TOTAL_COUNT) && total_count != histogram.hdr.total_count() {
        return None;
    }
    let min = if seen_keys.has(KEY_MIN) {
        min_value
    } else {
        histogram.hdr.min_value()
    };
    let max = if seen_keys.has(KEY_MAX) {
        max_value
    } else {
        histogram.hdr.max_value()
    };
    histogram.hdr.set_min_max(min, max);

    if ewma_alpha > 0.0 {
        histogram.ewma_mean = ewma_mean;
        histogram.ewma_variance = ewma_variance;
        histogram.ewma_error_rate = ewma_error_rate;
        histogram.ewma_initialized = true;
    }
    Some(histogram)
}

#[cfg(test)]
#[path = "cbor-test.rs"]
mod tests;
