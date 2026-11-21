// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The token key and the AES-256-GCM envelope of RPC tokens.

use channel_token_capnp::channel_token::RPC_TOKEN_MAGIC;
use ring::aead;
use ring::digest;
use ring::hmac;

use crate::Result;

/// The size of the AES-256 key.
pub const KEY_SIZE: usize = 32;
/// The size of the AES-GCM IV (nonce).
pub const IV_SIZE: usize = 12;
/// The size of the AES-GCM tag (MAC).
const MAC_SIZE: usize = 16;
/// The size of the key ID, the prefix of the SHA-256 hash of the key.
pub const KEY_ID_SIZE: usize = 16;
/// The size of the header of an RPC token: magic, IV and key ID. It is the AAD of the token.
const HEADER_SIZE: usize = 4 + IV_SIZE + KEY_ID_SIZE;

/// Labels the token key, so that it never equals another key derived from the cluster key.
const CLUSTER_KEY_LABEL: &[u8] = b"workerd channel token v1";

/// The key that seals and opens RPC tokens, and its key ID.
pub struct TokenKey {
    key: aead::LessSafeKey,
    key_id: [u8; KEY_ID_SIZE],
}

impl TokenKey {
    /// The key of a process: `secret` is random, so only this process redeems its tokens.
    #[must_use]
    pub fn from_secret(secret: &[u8; KEY_SIZE]) -> Self {
        let digest = digest::digest(&digest::SHA256, secret);
        let mut key_id = [0; KEY_ID_SIZE];
        key_id.copy_from_slice(&digest.as_ref()[..KEY_ID_SIZE]);
        let key = aead::UnboundKey::new(&aead::AES_256_GCM, secret)
            .unwrap_or_else(|_| unreachable!("an AES-256 key is {KEY_SIZE} bytes"));
        Self {
            key: aead::LessSafeKey::new(key),
            key_id,
        }
    }

    /// The key of a cluster, `HMAC-SHA256(key = label, message = cluster_key)`: the instances of a
    /// cluster share `cluster_key`, so each redeems the others' tokens.
    #[must_use]
    pub fn from_cluster_key(cluster_key: &[u8]) -> Self {
        let tag = hmac::sign(
            &hmac::Key::new(hmac::HMAC_SHA256, CLUSTER_KEY_LABEL),
            cluster_key,
        );
        let mut secret = [0; KEY_SIZE];
        secret.copy_from_slice(tag.as_ref());
        Self::from_secret(&secret)
    }

    /// The key ID, which an RPC token carries in the clear.
    #[must_use]
    pub fn key_id(&self) -> &[u8; KEY_ID_SIZE] {
        &self.key_id
    }

    /// Seals `plaintext` into an RPC token: the header (magic, little-endian; `iv`; key ID), the
    /// ciphertext, and the tag, which covers the ciphertext and the header as AAD.
    #[must_use]
    pub fn seal(&self, iv: &[u8; IV_SIZE], plaintext: &[u8]) -> Vec<u8> {
        let mut token = Vec::with_capacity(HEADER_SIZE + plaintext.len() + MAC_SIZE);
        token.extend_from_slice(&RPC_TOKEN_MAGIC.to_le_bytes());
        token.extend_from_slice(iv);
        token.extend_from_slice(&self.key_id);
        token.extend_from_slice(plaintext);
        let (header, body) = token.split_at_mut(HEADER_SIZE);
        let tag = self
            .key
            .seal_in_place_separate_tag(
                aead::Nonce::assume_unique_for_key(*iv),
                aead::Aad::from(&*header),
                body,
            )
            .unwrap_or_else(|_| unreachable!("a token is far shorter than AES-GCM's limit"));
        token.extend_from_slice(tag.as_ref());
        token
    }

    /// Opens an RPC token, returning its plaintext. The key ID is not checked on its own: a token
    /// of another key fails authentication, as does any change to it.
    ///
    /// # Errors
    ///
    /// Fails if the token is too short, does not start with the RPC magic number, or fails
    /// authentication.
    pub fn open(&self, token: &[u8]) -> Result<Vec<u8>> {
        if token.len() < HEADER_SIZE + MAC_SIZE {
            kj::fail_require!(
                "expected token.size() >= sizeof(header) + AES_MAC_SIZE; invalid channel token \
                 for RPC"
            );
        }
        let (header, sealed) = token.split_at(HEADER_SIZE);
        if header[..4] != RPC_TOKEN_MAGIC.to_le_bytes() {
            kj::fail_require!(
                "expected header.magic == ChannelToken::RPC_TOKEN_MAGIC; invalid channel token \
                 for RPC"
            );
        }
        let mut iv = [0; IV_SIZE];
        iv.copy_from_slice(&header[4..4 + IV_SIZE]);
        let mut body = sealed.to_vec();
        let plaintext_len = self
            .key
            .open_in_place(
                aead::Nonce::assume_unique_for_key(iv),
                aead::Aad::from(header),
                &mut body,
            )
            .map_err(|_| kj::failed!("channel token failed authentication"))?
            .len();
        body.truncate(plaintext_len);
        Ok(body)
    }
}

#[cfg(test)]
#[path = "key-test.rs"]
mod tests;
