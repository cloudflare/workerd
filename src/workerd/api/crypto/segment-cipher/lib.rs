// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A segmented authenticated cipher, for C++'s `crypto.SegmentEncryptionStream` and
//! `SegmentDecryptionStream` (`src/workerd/api/crypto/segment-stream.c++`).
//!
//! The plaintext is cut into segments of a fixed size (only the last may be shorter), and each
//! segment is sealed on its own under a key derived for its index. A reader can therefore start
//! at any segment, and decrypt as the data streams in, without holding the whole message. Tink's
//! streaming AEAD and age's STREAM are constructions of the same kind. See `layout` for the wire
//! format, `schedule` for the keys and `cipher` for what the caller must keep unique.
//!
//! The cryptographic primitives are `BoringSSL`'s, called through `primitives.h`; this crate only
//! composes them. C++ drives a [`SegmentTransform`] from a `SyncTransformStage`. Errors are
//! described as JSG expects (`jsg.TypeError: ...`) so they reach JavaScript as such.

mod cipher;
mod error;
mod layout;
mod schedule;
mod stages;

use std::pin::Pin;

use crate::cipher::Cipher;
use crate::cipher::Entropy;
use crate::error::Result;
use crate::error::not_supported_error;
use crate::error::range_error;
use crate::error::type_error;
use crate::layout::Algorithm;
use crate::layout::Layout;
use crate::schedule::IndexEncoding;
use crate::schedule::InfoTemplate;
use crate::schedule::KeySchedule;
use crate::schedule::Secret;
use crate::stages::Decryptor;
use crate::stages::Encryptor;
use crate::stages::OutputRange;
use crate::stages::require_segment_range;

#[cxx::bridge(namespace = "workerd::api::segment_cipher")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    /// The HKDF hash; `primitives.h` defines it.
    #[repr(u8)]
    enum Digest {
        Sha1,
        Sha256,
        Sha384,
        Sha512,
    }

    /// The AEADs; `primitives.h` defines it.
    #[repr(u8)]
    enum Aead {
        AesGcm,
        ChaCha20Poly1305,
    }

    /// A key info template, as JavaScript passed it.
    struct KeyInfo {
        prefix: Vec<u8>,
        /// "decimal", "uint32be" or "uint64be".
        index: String,
        suffix: Vec<u8>,
    }

    /// A stream's options, as JavaScript passed them. Each `has_` flag says whether the value
    /// after it was given. The HKDF key itself stays in C++, which passes its HKDF-Extract
    /// output instead.
    struct CipherOptions {
        algorithm: String,
        segment_size: f64,
        derive: String,
        /// The WebCrypto name of the HKDF hash, such as "SHA-256".
        hash: String,
        info: KeyInfo,
        has_mac_info: bool,
        mac_info: KeyInfo,
        has_key_bits: bool,
        key_bits: u32,
        has_first_segment: bool,
        first_segment: f64,
        has_plaintext_length: bool,
        plaintext_length: f64,
        has_output_offset: bool,
        output_offset: f64,
        has_output_length: bool,
        output_length: f64,
    }

    unsafe extern "C++" {
        include!("workerd/api/crypto/segment-cipher/primitives.h");

        type Digest;
        type Aead;
        type EntropySource;

        fn fill_random(source: Pin<&mut EntropySource>, out: &mut [u8]) -> Result<()>;
        fn hkdf_expand(digest: Digest, prk: &[u8], info: &[u8], out: &mut [u8]) -> Result<()>;
        fn aead_seal(
            aead: Aead,
            key: &[u8],
            nonce: &[u8],
            plaintext: &[u8],
            out: &mut [u8],
        ) -> Result<()>;
        fn aead_open(
            aead: Aead,
            key: &[u8],
            nonce: &[u8],
            sealed: &[u8],
            out: &mut [u8],
        ) -> Result<bool>;
        fn aes_256_cbc_encrypt(
            key: &[u8],
            iv: &[u8],
            plaintext: &[u8],
            out: &mut [u8],
        ) -> Result<()>;
        fn aes_256_cbc_decrypt(
            key: &[u8],
            iv: &[u8],
            ciphertext: &[u8],
            out: &mut [u8],
            written: &mut usize,
        ) -> Result<bool>;
        fn hmac_sha256(key: &[u8], data: &[u8], out: &mut [u8]) -> Result<()>;
        #[nothrow]
        fn constant_time_equal(a: &[u8], b: &[u8]) -> bool;
        #[nothrow]
        fn cleanse(bytes: &mut [u8]);
    }

    extern "Rust" {
        type SegmentTransform;

        /// Validates `options` and creates an encrypting transform. `prk` is HKDF-Extract of the
        /// key and salt.
        fn new_encryptor(options: CipherOptions, prk: &[u8]) -> Result<Box<SegmentTransform>>;

        /// Validates `options` and creates a decrypting transform.
        fn new_decryptor(options: CipherOptions, prk: &[u8]) -> Result<Box<SegmentTransform>>;

        /// The size `plaintext_length` bytes encrypt to.
        fn encrypted_length(
            plaintext_length: f64,
            algorithm: &str,
            segment_size: f64,
        ) -> Result<f64>;

        /// Transforms `input`. The output is taken with `take_output()`, also after a failure,
        /// when it is the output of the segments before the one that failed.
        fn push(
            self: &mut SegmentTransform,
            input: &[u8],
            entropy: Pin<&mut EntropySource>,
        ) -> Result<()>;

        /// Finishes the stream once the input ends.
        fn end(self: &mut SegmentTransform, entropy: Pin<&mut EntropySource>) -> Result<()>;

        /// Takes the output of the last `push()` or `end()`.
        fn take_output(self: &mut SegmentTransform) -> Vec<u8>;

        /// The output's total length, if it is known.
        fn output_length(self: &SegmentTransform) -> KjMaybe<u64>;

        /// The bytes the transform holds between calls, for memory accounting.
        fn retained_bytes(self: &SegmentTransform) -> usize;

        /// The write size the transform handles without copying: a segment when encrypting,
        /// a sealed segment when decrypting.
        fn high_water_mark(self: &SegmentTransform) -> usize;
    }
}

/// `Number.MAX_SAFE_INTEGER`.
const MAX_SAFE_INTEGER: u64 = (1 << 53) - 1;

/// Converts an option that must be a non-negative safe integer.
fn require_safe_integer(value: f64, name: &str) -> Result<u64> {
    #[expect(
        clippy::cast_precision_loss,
        reason = "2^53 - 1 is exactly representable"
    )]
    let max = MAX_SAFE_INTEGER as f64;
    if !(0.0..=max).contains(&value) || value.fract() != 0.0 {
        return Err(range_error(&format!(
            "{name} must be a non-negative integer no greater than Number.MAX_SAFE_INTEGER."
        )));
    }
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "the value is a non-negative integer of at most 2^53 - 1"
    )]
    Ok(value as u64)
}

fn optional_safe_integer(present: bool, value: f64, name: &str) -> Result<Option<u64>> {
    present
        .then(|| require_safe_integer(value, name))
        .transpose()
}

fn to_layout(algorithm: &str, segment_size: f64) -> Result<Layout> {
    let segment_size = require_safe_integer(segment_size, "segmentSize")?;
    Layout::new(Algorithm::parse(algorithm)?, segment_size)
}

fn to_info(info: ffi::KeyInfo) -> Result<InfoTemplate> {
    Ok(InfoTemplate {
        index: IndexEncoding::parse(&info.index)?,
        prefix: info.prefix,
        suffix: info.suffix,
    })
}

fn to_digest(hash: &str) -> Result<ffi::Digest> {
    match hash {
        "SHA-1" => Ok(ffi::Digest::Sha1),
        "SHA-256" => Ok(ffi::Digest::Sha256),
        "SHA-384" => Ok(ffi::Digest::Sha384),
        "SHA-512" => Ok(ffi::Digest::Sha512),
        _ => Err(not_supported_error(&format!(
            "Unrecognized segment key hash \"{hash}\"."
        ))),
    }
}

/// The options other than the cipher's.
struct StreamOptions {
    first_segment: u64,
    plaintext_length: Option<u64>,
    output_offset: Option<u64>,
    output_length: Option<u64>,
}

fn to_cipher(options: ffi::CipherOptions, prk: &[u8]) -> Result<(Cipher, StreamOptions)> {
    let layout = to_layout(&options.algorithm, options.segment_size)?;
    if options.derive != "HKDF" {
        return Err(not_supported_error(&format!(
            "Unrecognized segment key derivation \"{}\"; expected \"HKDF\".",
            options.derive
        )));
    }
    let keys = KeySchedule {
        digest: to_digest(&options.hash)?,
        prk: Secret::new(prk.to_vec()),
    };
    let info = to_info(options.info)?;
    let mac_info = options
        .has_mac_info
        .then(|| to_info(options.mac_info))
        .transpose()?;
    let key_bits = if options.has_key_bits {
        options.key_bits
    } else {
        256
    };
    let cipher = Cipher::new(layout, keys, info, mac_info, key_bits)?;
    let stream = StreamOptions {
        first_segment: optional_safe_integer(
            options.has_first_segment,
            options.first_segment,
            "firstSegment",
        )?
        .unwrap_or(0),
        plaintext_length: optional_safe_integer(
            options.has_plaintext_length,
            options.plaintext_length,
            "plaintextLength",
        )?,
        output_offset: optional_safe_integer(
            options.has_output_offset,
            options.output_offset,
            "outputOffset",
        )?,
        output_length: optional_safe_integer(
            options.has_output_length,
            options.output_length,
            "outputLength",
        )?,
    };
    Ok((cipher, stream))
}

fn require_safe_sealed_length(layout: &Layout, plaintext_length: u64) -> Result<u64> {
    let length = layout.sealed_length(plaintext_length);
    if length > MAX_SAFE_INTEGER {
        return Err(range_error(
            "The encrypted length would exceed Number.MAX_SAFE_INTEGER.",
        ));
    }
    Ok(length)
}

enum Stage {
    Encrypt(Encryptor),
    Decrypt(Decryptor),
}

/// An encrypting or decrypting transform, for C++.
pub struct SegmentTransform {
    stage: Stage,
    output: Vec<u8>,
}

/// Validates `options` and creates an encrypting transform.
///
/// # Errors
///
/// Fails with the JavaScript error the invalid option calls for.
pub fn new_encryptor(options: ffi::CipherOptions, prk: &[u8]) -> Result<Box<SegmentTransform>> {
    if options.has_output_offset || options.has_output_length {
        return Err(type_error(
            "outputOffset and outputLength are only used by SegmentDecryptionStream.",
        ));
    }
    let (cipher, stream) = to_cipher(options, prk)?;
    if let Some(length) = stream.plaintext_length {
        require_segment_range(
            &cipher,
            stream.first_segment,
            cipher.layout().segment_count(length),
        )?;
        require_safe_sealed_length(cipher.layout(), length)?;
    } else {
        require_segment_range(&cipher, stream.first_segment, 0)?;
    }
    Ok(Box::new(SegmentTransform {
        stage: Stage::Encrypt(Encryptor::new(
            cipher,
            stream.first_segment,
            stream.plaintext_length,
        )),
        output: Vec::new(),
    }))
}

/// Validates `options` and creates a decrypting transform.
///
/// # Errors
///
/// Fails with the JavaScript error the invalid option calls for.
pub fn new_decryptor(options: ffi::CipherOptions, prk: &[u8]) -> Result<Box<SegmentTransform>> {
    let (cipher, stream) = to_cipher(options, prk)?;
    let plaintext_length = stream.plaintext_length.ok_or_else(|| {
        type_error(
            "SegmentDecryptionStream requires plaintextLength, so that it can detect truncation.",
        )
    })?;
    let offset = stream.output_offset.unwrap_or(0);
    if offset > plaintext_length {
        return Err(range_error(&format!(
            "outputOffset ({offset}) is past the end of the plaintext ({plaintext_length})."
        )));
    }
    let end = stream.output_length.map_or(plaintext_length, |length| {
        offset + length.min(plaintext_length - offset)
    });
    require_segment_range(
        &cipher,
        stream.first_segment,
        cipher.layout().segment_count(plaintext_length),
    )?;
    Ok(Box::new(SegmentTransform {
        stage: Stage::Decrypt(Decryptor::new(
            cipher,
            stream.first_segment,
            plaintext_length,
            OutputRange { offset, end },
        )),
        output: Vec::new(),
    }))
}

/// The size `plaintext_length` bytes encrypt to.
///
/// # Errors
///
/// Fails with a `RangeError` for an invalid length or layout, or one that encrypts to more than
/// `Number.MAX_SAFE_INTEGER` bytes.
pub fn encrypted_length(plaintext_length: f64, algorithm: &str, segment_size: f64) -> Result<f64> {
    let layout = to_layout(algorithm, segment_size)?;
    let length = require_safe_sealed_length(
        &layout,
        require_safe_integer(plaintext_length, "plaintextLength")?,
    )?;
    #[expect(
        clippy::cast_precision_loss,
        reason = "the length is at most 2^53 - 1, so it is exact"
    )]
    Ok(length as f64)
}

/// The `IoContext`'s entropy source.
struct IoEntropy<'a>(Pin<&'a mut ffi::EntropySource>);

impl Entropy for IoEntropy<'_> {
    fn fill(&mut self, out: &mut [u8]) -> Result<()> {
        ffi::fill_random(self.0.as_mut(), out)?;
        Ok(())
    }
}

impl SegmentTransform {
    /// Transforms `input`.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if the input is invalid or fails authentication.
    pub fn push(&mut self, input: &[u8], entropy: Pin<&mut ffi::EntropySource>) -> Result<()> {
        // Sealing adds a few dozen bytes per segment, and opening removes them.
        let mut output = Vec::with_capacity(input.len() + self.high_water_mark() / 16);
        let result = match &mut self.stage {
            Stage::Encrypt(encryptor) => {
                encryptor.push(input, &mut IoEntropy(entropy), &mut output)
            }
            Stage::Decrypt(decryptor) => decryptor.push(input, &mut output),
        };
        self.output = output;
        result
    }

    /// Finishes the stream.
    ///
    /// # Errors
    ///
    /// Fails with an `OperationError` if the input ended early.
    pub fn end(&mut self, entropy: Pin<&mut ffi::EntropySource>) -> Result<()> {
        let mut output = Vec::new();
        let result = match &mut self.stage {
            Stage::Encrypt(encryptor) => encryptor.end(&mut IoEntropy(entropy), &mut output),
            Stage::Decrypt(decryptor) => decryptor.end(),
        };
        self.output = output;
        result
    }

    pub fn take_output(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.output)
    }

    #[must_use]
    pub fn output_length(&self) -> kj_rs::KjMaybe<u64> {
        match &self.stage {
            Stage::Encrypt(encryptor) => encryptor.output_length(),
            Stage::Decrypt(decryptor) => Some(decryptor.output_length()),
        }
        .into()
    }

    #[must_use]
    pub const fn retained_bytes(&self) -> usize {
        match &self.stage {
            Stage::Encrypt(encryptor) => encryptor.retained_bytes(),
            Stage::Decrypt(decryptor) => decryptor.retained_bytes(),
        }
    }

    #[must_use]
    pub const fn high_water_mark(&self) -> usize {
        match &self.stage {
            Stage::Encrypt(encryptor) => encryptor.cipher().layout().segment_size(),
            Stage::Decrypt(decryptor) => decryptor.cipher().layout().full_sealed_segment_size(),
        }
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
