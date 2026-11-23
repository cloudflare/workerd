// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The streaming halves: an encryptor that cuts plaintext into segments and seals each one, and
//! a decryptor that splits sealed input by the segments' known sizes, opens each one, and trims
//! the plaintext to the requested range.

use crate::cipher::Cipher;
use crate::cipher::Entropy;
use crate::error::Result;
use crate::error::operation_error;
use crate::error::range_error;
use crate::layout::to_u64;

/// A segment stream handles fewer than 2^32 segments, so that random 96-bit nonces stay
/// collision-safe even if a key schedule is reused.
pub const MAX_SEGMENTS_PER_STREAM: u64 = u32::MAX as u64;

/// Checks that `count` segments starting at `first_segment` can be keyed and that the stream
/// stays under its segment limit.
///
/// # Errors
///
/// Fails with a `RangeError` if they can't.
pub fn require_segment_range(cipher: &Cipher, first_segment: u64, count: u64) -> Result<()> {
    if count > MAX_SEGMENTS_PER_STREAM {
        return Err(range_error(&format!(
            "A segment stream can hold at most {MAX_SEGMENTS_PER_STREAM} segments."
        )));
    }
    // The last index, first_segment + count - 1, must not exceed max_index(), which may be
    // u64::MAX.
    let max = cipher.max_index();
    if first_segment > max || (count > 0 && count - 1 > max - first_segment) {
        return Err(range_error(
            "The segment indexes exceed what the key schedule's index encoding can hold.",
        ));
    }
    Ok(())
}

/// A size within one segment, which fits in memory.
fn to_size(value: u64) -> usize {
    usize::try_from(value).unwrap_or(usize::MAX)
}

/// Cuts the plaintext into segments and seals each one.
pub struct Encryptor {
    cipher: Cipher,
    first_segment: u64,
    plaintext_length: Option<u64>,
    /// Holds a partial segment. Unallocated until one is needed.
    buffer: Vec<u8>,
    received: u64,
    segments_sealed: u64,
}

impl Encryptor {
    #[must_use]
    pub const fn new(cipher: Cipher, first_segment: u64, plaintext_length: Option<u64>) -> Self {
        Self {
            cipher,
            first_segment,
            plaintext_length,
            buffer: Vec::new(),
            received: 0,
            segments_sealed: 0,
        }
    }

    #[must_use]
    pub const fn cipher(&self) -> &Cipher {
        &self.cipher
    }

    /// Seals the segments `input` completes, appending them to `out`.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if the input exceeds the plaintext length.
    pub fn push(
        &mut self,
        mut input: &[u8],
        entropy: &mut dyn Entropy,
        out: &mut Vec<u8>,
    ) -> Result<()> {
        self.received += to_u64(input.len());
        let received = self.received;
        if let Some(length) = self.plaintext_length.filter(|&length| received > length) {
            return Err(operation_error(&format!(
                "Received more than the plaintextLength ({length}) bytes of plaintext."
            )));
        }
        let segment_size = self.cipher.layout().segment_size();
        while !input.is_empty() {
            if self.buffer.is_empty() && input.len() >= segment_size {
                // A whole segment is in the input; seal it from there.
                let (segment, rest) = input.split_at(segment_size);
                self.seal_next(segment, entropy, out)?;
                input = rest;
                continue;
            }
            // Only input that doesn't fill a segment needs the buffer, so it's allocated when
            // that first happens.
            self.buffer.reserve_exact(segment_size - self.buffer.len());
            let n = (segment_size - self.buffer.len()).min(input.len());
            let (taken, rest) = input.split_at(n);
            self.buffer.extend_from_slice(taken);
            input = rest;
            if self.buffer.len() == segment_size {
                self.seal_buffer(entropy, out)?;
            }
        }
        Ok(())
    }

    /// Seals the last, partial, segment.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if the input fell short of the plaintext length.
    pub fn end(&mut self, entropy: &mut dyn Entropy, out: &mut Vec<u8>) -> Result<()> {
        let received = self.received;
        if let Some(length) = self.plaintext_length.filter(|&length| received != length) {
            return Err(operation_error(&format!(
                "Received {received} bytes of plaintext, but the plaintextLength is {length}."
            )));
        }
        if self.buffer.is_empty() {
            return Ok(());
        }
        self.seal_buffer(entropy, out)
    }

    #[must_use]
    pub fn output_length(&self) -> Option<u64> {
        self.plaintext_length
            .map(|length| self.cipher.layout().sealed_length(length))
    }

    #[must_use]
    pub const fn retained_bytes(&self) -> usize {
        self.buffer.capacity()
    }

    fn seal_buffer(&mut self, entropy: &mut dyn Entropy, out: &mut Vec<u8>) -> Result<()> {
        let buffer = std::mem::take(&mut self.buffer);
        let result = self.seal_next(&buffer, entropy, out);
        // Keep the allocation for the next segment.
        self.buffer = buffer;
        self.buffer.clear();
        result
    }

    fn seal_next(
        &mut self,
        plaintext: &[u8],
        entropy: &mut dyn Entropy,
        out: &mut Vec<u8>,
    ) -> Result<()> {
        require_segment_range(&self.cipher, self.first_segment, self.segments_sealed + 1)?;
        self.cipher.seal(
            self.first_segment + self.segments_sealed,
            plaintext,
            entropy,
            out,
        )?;
        self.segments_sealed += 1;
        Ok(())
    }
}

/// The decryptor's plaintext range: `[offset, end)`.
#[derive(Clone, Copy, Debug)]
pub struct OutputRange {
    pub offset: u64,
    pub end: u64,
}

/// Splits the input into sealed segments by their known sizes, opens each one, and trims the
/// plaintext to the requested range. Every segment is authenticated, including those whose
/// plaintext is trimmed away.
pub struct Decryptor {
    cipher: Cipher,
    first_segment: u64,
    plaintext_length: u64,
    range: OutputRange,
    expected_input: u64,
    /// Holds a partial segment. Unallocated until one is needed.
    buffer: Vec<u8>,
    received: u64,
    segments_opened: u64,
}

impl Decryptor {
    #[must_use]
    pub fn new(
        cipher: Cipher,
        first_segment: u64,
        plaintext_length: u64,
        range: OutputRange,
    ) -> Self {
        let expected_input = cipher.layout().sealed_length(plaintext_length);
        Self {
            cipher,
            first_segment,
            plaintext_length,
            range,
            expected_input,
            buffer: Vec::new(),
            received: 0,
            segments_opened: 0,
        }
    }

    #[must_use]
    pub const fn cipher(&self) -> &Cipher {
        &self.cipher
    }

    /// Opens the segments `input` completes, appending their plaintext within the range to
    /// `out`.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if a segment fails authentication or the input runs past
    /// the last segment.
    pub fn push(&mut self, mut input: &[u8], out: &mut Vec<u8>) -> Result<()> {
        while !input.is_empty() {
            if self.received >= self.expected_input {
                return Err(operation_error(&format!(
                    "The input continues past the end of the last segment (byte {}).",
                    self.expected_input
                )));
            }
            let needed = self.current_sealed_size();
            if self.buffer.is_empty() && input.len() >= needed {
                // A whole segment is in the input; open it from there.
                let (segment, rest) = input.split_at(needed);
                self.received += to_u64(needed);
                self.open_next(segment, out)?;
                input = rest;
                continue;
            }
            // Only input that doesn't hold a whole segment needs the buffer, so it's allocated
            // when that first happens.
            let full = self.cipher.layout().full_sealed_segment_size();
            self.buffer.reserve_exact(full - self.buffer.len());
            let n = (needed - self.buffer.len()).min(input.len());
            let (taken, rest) = input.split_at(n);
            self.buffer.extend_from_slice(taken);
            self.received += to_u64(n);
            input = rest;
            if self.buffer.len() == needed {
                let buffer = std::mem::take(&mut self.buffer);
                let result = self.open_next(&buffer, out);
                self.buffer = buffer;
                self.buffer.clear();
                result?;
            }
        }
        Ok(())
    }

    /// # Errors
    ///
    /// Fails with an `OperationError` if the input ended before the last segment did.
    pub fn end(&self) -> Result<()> {
        if self.received != self.expected_input {
            return Err(operation_error(&format!(
                "The input ended after {} bytes, in segment {}; expected {} bytes.",
                self.received,
                self.first_segment + self.segments_opened,
                self.expected_input
            )));
        }
        Ok(())
    }

    #[must_use]
    pub const fn output_length(&self) -> u64 {
        self.range.end - self.range.offset
    }

    #[must_use]
    pub const fn retained_bytes(&self) -> usize {
        self.buffer.capacity()
    }

    /// The plaintext size of the next segment.
    fn current_plaintext_size(&self) -> usize {
        self.cipher
            .layout()
            .segment_plaintext_size(self.plaintext_length, self.segments_opened)
    }

    /// The sealed size of the next segment.
    fn current_sealed_size(&self) -> usize {
        self.cipher
            .layout()
            .sealed_segment_size(self.current_plaintext_size())
    }

    fn open_next(&mut self, sealed: &[u8], out: &mut Vec<u8>) -> Result<()> {
        let start = self.segments_opened * to_u64(self.cipher.layout().segment_size());
        let index = self.first_segment + self.segments_opened;
        let expected = self.current_plaintext_size();
        let at = out.len();
        let size = self.cipher.open(index, sealed, out)?;
        self.segments_opened += 1;
        // Padding lets CBC segments of nearby sizes seal to the same size, so the authenticated
        // size must be checked too: otherwise a segment sealed for a different plaintextLength
        // would pass.
        if size != expected {
            out.truncate(at);
            return Err(operation_error(&format!(
                "Segment {index} holds {size} bytes of plaintext, but the plaintextLength \
                 requires {expected}."
            )));
        }
        // Keep the part of [start, start + size) within the output range.
        let end = start + to_u64(size);
        let from = start.max(self.range.offset);
        let to = end.min(self.range.end);
        if from >= to {
            out.truncate(at);
            return Ok(());
        }
        out.truncate(at + to_size(to - start));
        out.drain(at..at + to_size(from - start));
        Ok(())
    }
}

#[cfg(test)]
#[path = "stages-test.rs"]
mod tests;
