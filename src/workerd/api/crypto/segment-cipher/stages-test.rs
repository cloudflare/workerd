// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::cipher::tests::CountingEntropy;
use crate::cipher::tests::cipher;
use crate::layout::Algorithm;

const SEGMENT: usize = 1024;

fn plaintext(size: usize) -> Vec<u8> {
    (0..size).map(|i| u8::try_from(i % 251).unwrap()).collect()
}

/// Feeds `input` in writes of `chunk` bytes.
fn encrypt(algorithm: Algorithm, input: &[u8], chunk: usize) -> Vec<u8> {
    let mut encryptor = Encryptor::new(
        cipher(algorithm, to_u64(SEGMENT)),
        5,
        Some(to_u64(input.len())),
    );
    let mut entropy = CountingEntropy::default();
    let mut out = Vec::new();
    for piece in input.chunks(chunk) {
        encryptor.push(piece, &mut entropy, &mut out).unwrap();
    }
    encryptor.end(&mut entropy, &mut out).unwrap();
    assert_eq!(Some(to_u64(out.len())), encryptor.output_length());
    out
}

fn decryptor(algorithm: Algorithm, plaintext_length: usize, offset: u64, end: u64) -> Decryptor {
    Decryptor::new(
        cipher(algorithm, to_u64(SEGMENT)),
        5,
        to_u64(plaintext_length),
        OutputRange { offset, end },
    )
}

fn decrypt(decryptor: &mut Decryptor, input: &[u8], chunk: usize) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    for piece in input.chunks(chunk) {
        decryptor.push(piece, &mut out)?;
    }
    decryptor.end()?;
    Ok(out)
}

#[test]
fn round_trip_in_any_chunking() {
    for algorithm in [Algorithm::AesGcm, Algorithm::AesCbcHmacSha256] {
        for size in [0, 1, SEGMENT - 1, SEGMENT, 3 * SEGMENT + 17] {
            let input = plaintext(size);
            let sealed = encrypt(algorithm, &input, SEGMENT);
            for chunk in [1, 7, SEGMENT, SEGMENT + 1, 5 * SEGMENT] {
                assert_eq!(sealed.len(), encrypt(algorithm, &input, chunk).len());
                let mut decryptor = decryptor(algorithm, size, 0, to_u64(size));
                assert_eq!(
                    decrypt(&mut decryptor, &sealed, chunk).unwrap(),
                    input,
                    "{algorithm:?} {size} {chunk}"
                );
            }
        }
    }
}

#[test]
fn trims_to_the_range() {
    let input = plaintext(3 * SEGMENT + 17);
    let sealed = encrypt(Algorithm::AesGcm, &input, 100);
    let total = to_u64(input.len());
    for (offset, end) in [(0, 0), (10, 20), (1000, 2100), (1024, 2048), (3000, total)] {
        let mut decryptor = decryptor(Algorithm::AesGcm, input.len(), offset, end);
        assert_eq!(decryptor.output_length(), end - offset);
        let out = decrypt(&mut decryptor, &sealed, 333).unwrap();
        assert_eq!(out, &input[to_size(offset)..to_size(end)]);
    }
}

#[test]
fn detects_truncation_extension_and_reordering() {
    let input = plaintext(2 * SEGMENT + 5);
    let sealed = encrypt(Algorithm::AesGcm, &input, SEGMENT);
    let full = SEGMENT + 28;
    let all = to_u64(input.len());

    let mut truncated = decryptor(Algorithm::AesGcm, input.len(), 0, all);
    let error = decrypt(&mut truncated, &sealed[..2 * full], SEGMENT).unwrap_err();
    assert!(
        error.description().contains("input ended after"),
        "{error:?}"
    );

    let mut extended = sealed.clone();
    extended.push(0);
    let mut decryptor_ = decryptor(Algorithm::AesGcm, input.len(), 0, all);
    let error = decrypt(&mut decryptor_, &extended, SEGMENT).unwrap_err();
    assert!(
        error.description().contains("continues past the end"),
        "{error:?}"
    );

    let mut swapped = sealed[full..2 * full].to_vec();
    swapped.extend_from_slice(&sealed[..full]);
    swapped.extend_from_slice(&sealed[2 * full..]);
    let mut decryptor_ = decryptor(Algorithm::AesGcm, input.len(), 0, all);
    let mut out = Vec::new();
    let error = decryptor_.push(&swapped, &mut out).unwrap_err();
    assert!(
        error.description().contains("Segment 5 failed"),
        "{error:?}"
    );
}

// CBC pads, so a last segment sealed for 20 bytes has the size of one sealed for 17.
#[test]
fn checks_the_authenticated_size() {
    let sealed = encrypt(
        Algorithm::AesCbcHmacSha256,
        &plaintext(SEGMENT + 20),
        SEGMENT,
    );
    let mut decryptor = decryptor(Algorithm::AesCbcHmacSha256, SEGMENT + 17, 0, 0);
    let error = decrypt(&mut decryptor, &sealed, SEGMENT).unwrap_err();
    assert!(
        error
            .description()
            .contains("holds 20 bytes of plaintext, but the plaintextLength requires 17"),
        "{error:?}"
    );
}

#[test]
fn checks_the_plaintext_length() {
    let mut entropy = CountingEntropy::default();
    let mut out = Vec::new();
    let mut encryptor = Encryptor::new(cipher(Algorithm::AesGcm, 1024), 0, Some(10));
    encryptor.push(&[0; 5], &mut entropy, &mut out).unwrap();
    assert!(encryptor.end(&mut entropy, &mut out).is_err());
    assert!(encryptor.push(&[0; 6], &mut entropy, &mut out).is_err());
}

#[test]
fn segment_range() {
    let gcm = cipher(Algorithm::AesGcm, 1024);
    assert!(require_segment_range(&gcm, u64::MAX, 1).is_ok());
    assert!(require_segment_range(&gcm, u64::MAX, 2).is_err());
    assert!(require_segment_range(&gcm, 0, MAX_SEGMENTS_PER_STREAM).is_ok());
    assert!(require_segment_range(&gcm, 0, MAX_SEGMENTS_PER_STREAM + 1).is_err());
    let cbc = cipher(Algorithm::AesCbcHmacSha256, 1024);
    assert!(require_segment_range(&cbc, u64::from(u32::MAX), 1).is_ok());
    assert!(require_segment_range(&cbc, u64::from(u32::MAX) + 1, 0).is_err());
}
