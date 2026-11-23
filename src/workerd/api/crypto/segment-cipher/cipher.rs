// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Seals and opens individual segments.
//!
//! Each segment's key is `HKDF-Expand(prk, info(index))`, and the nonce or IV beside it is
//! random. The segment index authenticates a segment's position, and the decrypting side checks
//! each segment's plaintext size against the `plaintextLength` it is given. Nothing else ties a
//! segment to its message, so the caller must keep two things:
//!
//! - The key schedule (key, salt and info) must be unique to each message, and to each version
//!   of a message that is rewritten under the same name: for example a fresh random salt for
//!   every encryption, or an info that carries an authenticated version identifier. Otherwise
//!   segments at the same index of two messages, or two versions, are interchangeable, and so are
//!   whole messages that end at the same segment.
//! - The `plaintextLength` given to the decrypting side must come from metadata that is itself
//!   authenticated or trusted, such as the record that holds the salt. It is what detects
//!   truncation and extension; the ciphertext cannot.

use crate::error::Result;
use crate::error::operation_error;
use crate::error::type_error;
use crate::ffi;
use crate::layout::AEAD_NONCE_SIZE;
use crate::layout::AEAD_TAG_SIZE;
use crate::layout::Algorithm;
use crate::layout::CBC_BLOCK_SIZE;
use crate::layout::HMAC_SHA256_SIZE;
use crate::layout::Layout;
use crate::schedule::InfoTemplate;
use crate::schedule::KeySchedule;

/// The source of nonces and IVs.
pub trait Entropy {
    /// Fills `out` with random bytes.
    ///
    /// # Errors
    ///
    /// Fails if the source does.
    fn fill(&mut self, out: &mut [u8]) -> Result<()>;
}

/// How segments are sealed.
enum Construction {
    Aead(ffi::Aead),
    /// Encrypt-then-MAC, with an HMAC key derived from its own info template.
    CbcHmac {
        mac_info: InfoTemplate,
    },
}

pub struct Cipher {
    layout: Layout,
    keys: KeySchedule,
    info: InfoTemplate,
    construction: Construction,
    key_size: usize,
}

const MAC_INFO_UNUSED: &str = "macInfo is only used by AES-CBC-HMAC-SHA256.";

fn authentication_failure(index: u64) -> cxx::KjError {
    operation_error(&format!("Segment {index} failed authentication."))
}

impl Cipher {
    /// `key_bits` is the AES key size for AES-GCM (128 or 256); the other algorithms have fixed
    /// key sizes and require 256.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` for a key size the algorithm doesn't have, and a
    /// `TypeError` if `mac_info` is given without AES-CBC-HMAC-SHA256 or missing with it.
    pub fn new(
        layout: Layout,
        keys: KeySchedule,
        info: InfoTemplate,
        mac_info: Option<InfoTemplate>,
        key_bits: u32,
    ) -> Result<Self> {
        let construction = match (layout.algorithm(), mac_info) {
            (Algorithm::AesGcm, None) => {
                if key_bits != 128 && key_bits != 256 {
                    return Err(operation_error(&format!(
                        "AES-GCM segment keys must be 128 or 256 bits long (requested {key_bits})."
                    )));
                }
                Construction::Aead(ffi::Aead::AesGcm)
            }
            (Algorithm::ChaCha20Poly1305, None) => {
                if key_bits != 256 {
                    return Err(operation_error(&format!(
                        "ChaCha20-Poly1305 segment keys are 256 bits long (requested {key_bits})."
                    )));
                }
                Construction::Aead(ffi::Aead::ChaCha20Poly1305)
            }
            (Algorithm::AesCbcHmacSha256, Some(mac_info)) => {
                if key_bits != 256 {
                    return Err(operation_error(&format!(
                        "AES-CBC-HMAC-SHA256 segment keys are 256 bits long (requested \
                         {key_bits})."
                    )));
                }
                Construction::CbcHmac { mac_info }
            }
            (Algorithm::AesGcm | Algorithm::ChaCha20Poly1305, Some(_)) => {
                return Err(type_error(MAC_INFO_UNUSED));
            }
            (Algorithm::AesCbcHmacSha256, None) => {
                return Err(type_error("AES-CBC-HMAC-SHA256 requires macInfo."));
            }
        };
        Ok(Self {
            layout,
            keys,
            info,
            construction,
            key_size: if key_bits == 128 { 16 } else { 32 },
        })
    }

    #[must_use]
    pub const fn layout(&self) -> &Layout {
        &self.layout
    }

    /// The largest segment index the key schedule can derive a key for.
    #[must_use]
    pub fn max_index(&self) -> u64 {
        let max = self.info.index.max_index();
        match &self.construction {
            Construction::Aead(_) => max,
            Construction::CbcHmac { mac_info } => max.min(mac_info.index.max_index()),
        }
    }

    /// Seals the plaintext of segment `index`, which must be non-empty and at most the segment
    /// size, appending it to `out`. On failure `out` is left as it was.
    ///
    /// # Errors
    ///
    /// Fails if `BoringSSL` or `entropy` does.
    pub fn seal(
        &self,
        index: u64,
        plaintext: &[u8],
        entropy: &mut dyn Entropy,
        out: &mut Vec<u8>,
    ) -> Result<()> {
        debug_assert!(!plaintext.is_empty() && plaintext.len() <= self.layout.segment_size());
        let start = out.len();
        out.resize(start + self.layout.sealed_segment_size(plaintext.len()), 0);
        let result = self.seal_into(index, plaintext, entropy, &mut out[start..]);
        if result.is_err() {
            out.truncate(start);
        }
        result
    }

    fn seal_into(
        &self,
        index: u64,
        plaintext: &[u8],
        entropy: &mut dyn Entropy,
        sealed: &mut [u8],
    ) -> Result<()> {
        let key = self.keys.derive(&self.info, index, self.key_size)?;
        match &self.construction {
            Construction::Aead(aead) => {
                let (nonce, ciphertext) = sealed.split_at_mut(AEAD_NONCE_SIZE);
                entropy.fill(nonce)?;
                ffi::aead_seal(*aead, key.as_slice(), nonce, plaintext, ciphertext)?;
            }
            Construction::CbcHmac { mac_info } => {
                let signed_size = sealed.len() - HMAC_SHA256_SIZE;
                let (signed, mac) = sealed.split_at_mut(signed_size);
                let (iv, ciphertext) = signed.split_at_mut(CBC_BLOCK_SIZE);
                entropy.fill(iv)?;
                ffi::aes_256_cbc_encrypt(key.as_slice(), iv, plaintext, ciphertext)?;
                let mac_key = self.keys.derive(mac_info, index, HMAC_SHA256_SIZE)?;
                ffi::hmac_sha256(mac_key.as_slice(), signed, mac)?;
            }
        }
        Ok(())
    }

    /// Opens sealed segment `index`, appending its plaintext to `out` and returning the
    /// plaintext's size. On failure `out` is left as it was.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if the segment fails authentication.
    pub fn open(&self, index: u64, sealed: &[u8], out: &mut Vec<u8>) -> Result<usize> {
        let start = out.len();
        let result = self.open_into(index, sealed, out, start);
        match result {
            Ok(()) => Ok(out.len() - start),
            Err(error) => {
                out.truncate(start);
                Err(error)
            }
        }
    }

    fn open_into(&self, index: u64, sealed: &[u8], out: &mut Vec<u8>, start: usize) -> Result<()> {
        let key = self.keys.derive(&self.info, index, self.key_size)?;
        match &self.construction {
            Construction::Aead(aead) => {
                if sealed.len() <= AEAD_NONCE_SIZE + AEAD_TAG_SIZE {
                    return Err(authentication_failure(index));
                }
                let (nonce, ciphertext) = sealed.split_at(AEAD_NONCE_SIZE);
                out.resize(start + ciphertext.len() - AEAD_TAG_SIZE, 0);
                if !ffi::aead_open(*aead, key.as_slice(), nonce, ciphertext, &mut out[start..])? {
                    return Err(authentication_failure(index));
                }
            }
            Construction::CbcHmac { mac_info } => {
                // The smallest segment is one IV block, one ciphertext block and the MAC.
                if sealed.len() < 2 * CBC_BLOCK_SIZE + HMAC_SHA256_SIZE
                    || !(sealed.len() - HMAC_SHA256_SIZE).is_multiple_of(CBC_BLOCK_SIZE)
                {
                    return Err(authentication_failure(index));
                }
                let (signed, expected_mac) = sealed.split_at(sealed.len() - HMAC_SHA256_SIZE);
                // Encrypt-then-MAC: nothing is decrypted until the MAC verifies.
                let mac_key = self.keys.derive(mac_info, index, HMAC_SHA256_SIZE)?;
                let mut mac = [0; HMAC_SHA256_SIZE];
                ffi::hmac_sha256(mac_key.as_slice(), signed, &mut mac)?;
                if !ffi::constant_time_equal(&mac, expected_mac) {
                    return Err(authentication_failure(index));
                }
                let (iv, ciphertext) = signed.split_at(CBC_BLOCK_SIZE);
                out.resize(start + ciphertext.len(), 0);
                let mut written = 0;
                if !ffi::aes_256_cbc_decrypt(
                    key.as_slice(),
                    iv,
                    ciphertext,
                    &mut out[start..],
                    &mut written,
                )? {
                    // The MAC verified, so the writer padded it wrongly.
                    return Err(authentication_failure(index));
                }
                out.truncate(start + written);
            }
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "cipher-test.rs"]
pub mod tests;
