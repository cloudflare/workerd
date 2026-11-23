// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The sizes of a segmented message, which depend only on the algorithm and the segment size.
//!
//! Each sealed segment carries its own random nonce:
//!
//! ```text
//! AES-GCM, ChaCha20-Poly1305:  nonce(12) || ciphertext || tag(16)
//! AES-CBC-HMAC-SHA256:         iv(16) || AES-CBC-PKCS7(plaintext) || HMAC-SHA256(iv || ct)(32)
//! ```

use crate::error::Result;
use crate::error::not_supported_error;
use crate::error::range_error;

pub const AEAD_NONCE_SIZE: usize = 12;
pub const AEAD_TAG_SIZE: usize = 16;
pub const CBC_BLOCK_SIZE: usize = 16;
pub const HMAC_SHA256_SIZE: usize = 32;

/// The supported range of `segmentSize`. The upper bound bounds the memory a stream holds.
pub const MIN_SEGMENT_SIZE: usize = 1024;
pub const MAX_SEGMENT_SIZE: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Algorithm {
    AesGcm,
    ChaCha20Poly1305,
    AesCbcHmacSha256,
}

impl Algorithm {
    /// Parses an algorithm name.
    ///
    /// # Errors
    ///
    /// Fails with a `NotSupportedError` for an unknown name.
    pub fn parse(name: &str) -> Result<Self> {
        match name {
            "AES-GCM" => Ok(Self::AesGcm),
            "ChaCha20-Poly1305" => Ok(Self::ChaCha20Poly1305),
            "AES-CBC-HMAC-SHA256" => Ok(Self::AesCbcHmacSha256),
            _ => Err(not_supported_error(&format!(
                "Unrecognized segment cipher algorithm \"{name}\"."
            ))),
        }
    }
}

/// Converts a size that fits in memory to a length.
pub const fn to_u64(size: usize) -> u64 {
    size as u64
}

#[derive(Clone, Copy, Debug)]
pub struct Layout {
    algorithm: Algorithm,
    segment_size: usize,
}

impl Layout {
    /// # Errors
    ///
    /// Fails with a `RangeError` if `segment_size` is out of range.
    pub fn new(algorithm: Algorithm, segment_size: u64) -> Result<Self> {
        match usize::try_from(segment_size) {
            Ok(size) if (MIN_SEGMENT_SIZE..=MAX_SEGMENT_SIZE).contains(&size) => Ok(Self {
                algorithm,
                segment_size: size,
            }),
            _ => Err(range_error(&format!(
                "segmentSize must be between {MIN_SEGMENT_SIZE} and {MAX_SEGMENT_SIZE} bytes \
                 (got {segment_size})."
            ))),
        }
    }

    #[must_use]
    pub const fn algorithm(&self) -> Algorithm {
        self.algorithm
    }

    #[must_use]
    pub const fn segment_size(&self) -> usize {
        self.segment_size
    }

    /// The size of one sealed segment holding `plaintext_size` bytes, at most the segment size.
    /// An empty plaintext is never sealed, so its size is 0.
    #[must_use]
    pub const fn sealed_segment_size(&self, plaintext_size: usize) -> usize {
        if plaintext_size == 0 {
            return 0;
        }
        match self.algorithm {
            Algorithm::AesGcm | Algorithm::ChaCha20Poly1305 => {
                AEAD_NONCE_SIZE + plaintext_size + AEAD_TAG_SIZE
            }
            // PKCS#7 always pads, adding a whole block when the plaintext fills its last one.
            Algorithm::AesCbcHmacSha256 => {
                CBC_BLOCK_SIZE
                    + (plaintext_size / CBC_BLOCK_SIZE + 1) * CBC_BLOCK_SIZE
                    + HMAC_SHA256_SIZE
            }
        }
    }

    #[must_use]
    pub const fn full_sealed_segment_size(&self) -> usize {
        self.sealed_segment_size(self.segment_size)
    }

    /// The number of segments a plaintext of `plaintext_length` bytes is cut into.
    #[must_use]
    pub const fn segment_count(&self, plaintext_length: u64) -> u64 {
        plaintext_length.div_ceil(to_u64(self.segment_size))
    }

    /// The plaintext size of segment `index` (counted from the start of the message) of a
    /// plaintext of `plaintext_length` bytes.
    #[must_use]
    pub fn segment_plaintext_size(&self, plaintext_length: u64, index: u64) -> usize {
        let segment_size = to_u64(self.segment_size);
        let start = index.saturating_mul(segment_size);
        let size = plaintext_length.saturating_sub(start).min(segment_size);
        // At most the segment size, which is a usize.
        usize::try_from(size).unwrap_or(self.segment_size)
    }

    /// The total sealed size of a plaintext of `plaintext_length` bytes.
    #[must_use]
    pub fn sealed_length(&self, plaintext_length: u64) -> u64 {
        let segment_size = to_u64(self.segment_size);
        let full_segments = plaintext_length / segment_size;
        let remainder = self.segment_plaintext_size(plaintext_length, full_segments);
        full_segments * to_u64(self.full_sealed_segment_size())
            + to_u64(self.sealed_segment_size(remainder))
    }
}

#[cfg(test)]
#[path = "layout-test.rs"]
mod tests;
