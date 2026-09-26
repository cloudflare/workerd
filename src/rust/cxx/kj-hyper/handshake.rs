//! The WebSocket handshake's computed header values (RFC 6455 section 4), and the entropy kj's
//! WebSocket masks frames with.
//!
//! Handshake policy (which requests are WebSocket upgrades, why one is refused, what
//! `Sec-WebSocket-Extensions` agrees to) is kj's, applied in kj-hyper.c++; this module holds what
//! kj keeps private.

use base64::Engine;
use sha1::Digest;

const GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// `Sec-WebSocket-Accept` for a `Sec-WebSocket-Key`.
#[must_use]
pub fn accept_key(key: &[u8]) -> String {
    let digest = sha1::Sha1::new()
        .chain_update(key)
        .chain_update(GUID)
        .finalize();
    base64::engine::general_purpose::STANDARD.encode(digest)
}

/// A fresh `Sec-WebSocket-Key`: 16 random bytes, base64.
#[must_use]
pub fn client_key() -> String {
    let mut nonce = [0u8; 16];
    rand::fill(&mut nonce);
    base64::engine::general_purpose::STANDARD.encode(nonce)
}

/// Fills `buffer` with cryptographically random bytes (kj's `EntropySource`, for frame masks).
pub fn fill_random(buffer: &mut [u8]) {
    rand::fill(buffer);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accept_key_is_rfc_6455s_example() {
        assert_eq!(
            accept_key(b"dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        );
    }

    #[test]
    fn client_keys_are_16_bytes_and_differ() {
        let a = client_key();
        let b = client_key();
        assert_eq!(a.len(), 24);
        assert!(a.ends_with("=="));
        assert_ne!(a, b);
    }
}
