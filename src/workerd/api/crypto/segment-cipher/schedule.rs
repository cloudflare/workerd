// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The key schedule: the key for segment `i` is `HKDF-Expand(prk, info(i))`, where `prk` is
//! HKDF-Extract of the caller's HKDF key and `info(i) = prefix || encode(i) || suffix`.
//! AES-CBC-HMAC-SHA256 derives its HMAC key the same way from a second info template.

use crate::error::Result;
use crate::error::type_error;
use crate::ffi;

/// How a segment index is written into the HKDF info.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IndexEncoding {
    /// ASCII decimal digits, without leading zeros.
    Decimal,
    Uint32Be,
    Uint64Be,
}

impl IndexEncoding {
    /// # Errors
    ///
    /// Fails with a `TypeError` for an unknown name.
    pub fn parse(name: &str) -> Result<Self> {
        match name {
            "decimal" => Ok(Self::Decimal),
            "uint32be" => Ok(Self::Uint32Be),
            "uint64be" => Ok(Self::Uint64Be),
            _ => Err(type_error(&format!(
                "Unrecognized segment index encoding \"{name}\"; expected \"decimal\", \
                 \"uint32be\" or \"uint64be\"."
            ))),
        }
    }

    /// The largest segment index the encoding can represent.
    #[must_use]
    pub const fn max_index(self) -> u64 {
        match self {
            Self::Uint32Be => u32::MAX as u64,
            Self::Decimal | Self::Uint64Be => u64::MAX,
        }
    }
}

/// The HKDF info for each segment: `prefix || encode(index) || suffix`.
#[derive(Clone, Debug)]
pub struct InfoTemplate {
    pub prefix: Vec<u8>,
    pub index: IndexEncoding,
    pub suffix: Vec<u8>,
}

impl InfoTemplate {
    /// The info for segment `index`, which must be at most the encoding's `max_index()`.
    #[must_use]
    pub fn build(&self, index: u64) -> Vec<u8> {
        debug_assert!(index <= self.index.max_index());
        let mut info = Vec::with_capacity(self.prefix.len() + 20 + self.suffix.len());
        info.extend_from_slice(&self.prefix);
        match self.index {
            IndexEncoding::Decimal => info.extend_from_slice(index.to_string().as_bytes()),
            IndexEncoding::Uint32Be => {
                // The index is at most u32::MAX here, so this keeps its low four bytes.
                info.extend_from_slice(&index.to_be_bytes()[4..]);
            }
            IndexEncoding::Uint64Be => info.extend_from_slice(&index.to_be_bytes()),
        }
        info.extend_from_slice(&self.suffix);
        info
    }
}

/// Key material, overwritten with zeros when it is dropped.
pub struct Secret(Vec<u8>);

impl Secret {
    #[must_use]
    pub const fn new(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }

    #[must_use]
    pub fn zeroed(size: usize) -> Self {
        Self(vec![0; size])
    }

    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.0
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.0
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        ffi::cleanse(&mut self.0);
    }
}

/// Derives segment keys: `HKDF-Expand(prk, info)`.
pub struct KeySchedule {
    pub digest: ffi::Digest,
    /// The HKDF pseudorandom key.
    pub prk: Secret,
}

impl KeySchedule {
    /// Derives the `size`-byte key for segment `index` from `info`.
    ///
    /// # Errors
    ///
    /// Fails if `BoringSSL` does.
    pub fn derive(&self, info: &InfoTemplate, index: u64, size: usize) -> Result<Secret> {
        let mut key = Secret::zeroed(size);
        ffi::hkdf_expand(
            self.digest,
            self.prk.as_slice(),
            &info.build(index),
            key.as_mut_slice(),
        )?;
        Ok(key)
    }
}

#[cfg(test)]
#[path = "schedule-test.rs"]
mod tests;
