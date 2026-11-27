// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

const IV: [u8; IV_SIZE] = [7; IV_SIZE];

fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    bytes.iter().fold(String::new(), |mut s, b| {
        let _ = write!(s, "{b:02x}");
        s
    })
}

#[test]
fn key_id_is_a_prefix_of_the_keys_sha256() {
    let key = TokenKey::from_secret(&[0; KEY_SIZE]);
    // SHA-256 of 32 zero bytes.
    assert_eq!(hex(key.key_id()), "66687aadf862bd776c8fc18b8e9f8e20");
}

#[test]
fn cluster_key_is_the_labeled_hmac_of_the_cluster_key() {
    let secret = hmac::sign(
        &hmac::Key::new(hmac::HMAC_SHA256, b"workerd channel token v1"),
        b"cluster",
    );
    let derived = TokenKey::from_cluster_key(b"cluster");
    let direct = TokenKey::from_secret(secret.as_ref().try_into().unwrap());
    assert_eq!(derived.key_id(), direct.key_id());
    assert_eq!(direct.open(&derived.seal(&IV, b"hello")).unwrap(), b"hello");
    assert_ne!(
        TokenKey::from_cluster_key(b"other").key_id(),
        derived.key_id()
    );
}

#[test]
fn seal_lays_out_magic_iv_key_id_ciphertext_and_tag() {
    let key = TokenKey::from_cluster_key(b"cluster");
    let token = key.seal(&IV, b"hello");
    assert_eq!(token.len(), HEADER_SIZE + 5 + MAC_SIZE);
    assert_eq!(token[..4], RPC_TOKEN_MAGIC.to_le_bytes());
    assert_eq!(token[4..16], IV);
    assert_eq!(token[16..32], *key.key_id());
    assert_ne!(&token[32..37], b"hello");
    assert_eq!(key.open(&token).unwrap(), b"hello");
}

#[test]
fn open_rejects_any_changed_byte() {
    let key = TokenKey::from_cluster_key(b"cluster");
    let token = key.seal(&IV, b"hello");
    for i in 0..token.len() {
        let mut copy = token.clone();
        copy[i] ^= 1;
        let message = key.open(&copy).unwrap_err().description().to_owned();
        let expected = if i < 4 {
            "RPC_TOKEN_MAGIC"
        } else {
            "channel token failed authentication"
        };
        assert!(message.contains(expected), "byte {i}: {message}");
    }
}

#[test]
fn open_rejects_another_keys_token() {
    let token = TokenKey::from_cluster_key(b"a").seal(&IV, b"hello");
    let message = TokenKey::from_cluster_key(b"b")
        .open(&token)
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("failed authentication"), "{message}");
}

#[test]
fn open_rejects_a_truncated_token() {
    let key = TokenKey::from_cluster_key(b"cluster");
    let token = key.seal(&IV, b"");
    assert_eq!(key.open(&token).unwrap(), b"");
    for len in 0..token.len() {
        let message = key
            .open(&token[..len])
            .unwrap_err()
            .description()
            .to_owned();
        assert!(
            message.contains("invalid channel token for RPC"),
            "{len}: {message}"
        );
    }
    let token = key.seal(&IV, b"hello");
    let message = key
        .open(&token[..token.len() - 1])
        .unwrap_err()
        .description()
        .to_owned();
    assert!(message.contains("failed authentication"), "{message}");
}
