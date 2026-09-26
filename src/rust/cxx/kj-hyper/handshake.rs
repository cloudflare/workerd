//! The WebSocket handshake's computed header values (RFC 6455 section 4), and the entropy kj's
//! WebSocket masks frames with.
//!
//! `WebSocket`s are kj's: kj-hyper does the handshake (here, the accept and client keys) and
//! hands the upgraded transport to `kj::newWebSocket`, with frame-mask entropy from `rand`
//! (`RustEntropySource`). Handshake policy (which requests are WebSocket upgrades, why one is
//! refused, what `Sec-WebSocket-Extensions` agrees to, by kj's own parser) is kj's, applied in
//! kj-hyper.c++; this module holds what kj keeps private. Where the handshake differs from kj:
//! an unsupported `Sec-WebSocket-Version` is refused with `426` and `Sec-WebSocket-Version: 13`
//! (kj: `400`), and a handshake by POST with `400`.

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use sha1::Digest;

const GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// `Sec-WebSocket-Accept` for a `Sec-WebSocket-Key`.
#[must_use]
pub fn accept_key(key: &[u8]) -> String {
    let digest = sha1::Sha1::new()
        .chain_update(key)
        .chain_update(GUID)
        .finalize();
    STANDARD.encode(digest)
}

/// A fresh `Sec-WebSocket-Key`: 16 random bytes, base64.
#[must_use]
pub fn client_key() -> String {
    let mut nonce = [0u8; 16];
    rand::fill(&mut nonce);
    STANDARD.encode(nonce)
}

/// Fills `buffer` with cryptographically random bytes (kj's `EntropySource`, for frame masks).
pub fn fill_random(buffer: &mut [u8]) {
    rand::fill(buffer);
}

#[cfg(test)]
#[path = "handshake-test.rs"]
mod tests;
