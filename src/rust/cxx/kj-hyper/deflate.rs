//! permessage-deflate (RFC 7692): per-direction compression of message payloads.
//!
//! Negotiation happens in C++ with kj's own helpers; `crate::ws` applies these codecs to messages
//! whose first frame has RSV1 set, under the agreed parameters.

use crate::ffi::WsCompression;

/// The RFC 7692 tail a sender strips and a receiver restores.
const TAIL: [u8; 4] = [0x00, 0x00, 0xFF, 0xFF];

/// A protocol failure found while decompressing: the Close code and reason to fail with.
pub type Violation = (u16, String);

/// zlib cannot deflate with a window of 8 bits; 9 is wire-compatible for any receiver
/// (chromium bug 691074, as kj). flate2 also inflates with at least 9, which strictly contains 8.
fn window_bits(bits: u8) -> u8 {
    match bits {
        0 => 15,
        8 => 9,
        b => b,
    }
}

pub struct Deflater {
    ctx: flate2::Compress,
    reset_per_message: bool,
}

impl Deflater {
    pub fn new(params: &WsCompression) -> Self {
        Self {
            ctx: flate2::Compress::new_with_window_bits(
                flate2::Compression::default(),
                false,
                window_bits(params.outbound_max_window_bits),
            ),
            reset_per_message: params.outbound_no_context_takeover,
        }
    }

    /// One message's frame payload: sync-flushed with the tail stripped, or a single empty block
    /// for an empty message (RFC 7692 7.2.3.6), as kj sends.
    pub fn compress(&mut self, message: &[u8]) -> Result<Vec<u8>, String> {
        if self.reset_per_message {
            self.ctx.reset();
        }
        if message.is_empty() {
            return Ok(vec![0x00]);
        }
        let mut out = Vec::with_capacity(message.len() / 2 + 16);
        let mut consumed = 0;
        loop {
            out.reserve(4096);
            let (total_in, len, space) =
                (self.ctx.total_in(), out.len(), out.capacity() - out.len());
            self.ctx
                .compress_vec(&message[consumed..], &mut out, flate2::FlushCompress::Sync)
                .map_err(|e| format!("Error compressing websocket message: {e}"))?;
            consumed += usize::try_from(self.ctx.total_in() - total_in).unwrap_or(usize::MAX);
            // Done once all input is in and the flush did not fill the space it was given.
            if consumed >= message.len() && out.len() - len < space {
                break;
            }
        }
        if !out.ends_with(&TAIL) {
            return Err("Error compressing websocket message: no sync flush marker".to_owned());
        }
        out.truncate(out.len() - TAIL.len());
        Ok(out)
    }
}

pub struct Inflater {
    ctx: flate2::Decompress,
    reset_per_message: bool,
}

impl Inflater {
    pub fn new(params: &WsCompression) -> Self {
        Self {
            ctx: flate2::Decompress::new_with_window_bits(
                false,
                window_bits(params.inbound_max_window_bits),
            ),
            reset_per_message: params.inbound_no_context_takeover,
        }
    }

    /// One message's payload, decompressed up to `max_size` bytes.
    pub fn decompress(
        &mut self,
        mut message: Vec<u8>,
        max_size: usize,
    ) -> Result<Vec<u8>, Violation> {
        message.extend_from_slice(&TAIL);
        if self.reset_per_message {
            self.ctx.reset(false);
        }
        let mut out = Vec::new();
        let mut consumed = 0;
        loop {
            out.reserve(4096);
            let (total_in, len, space) =
                (self.ctx.total_in(), out.len(), out.capacity() - out.len());
            let status = self
                .ctx
                .decompress_vec(
                    &message[consumed..],
                    &mut out,
                    flate2::FlushDecompress::Sync,
                )
                .map_err(|_| (1002, "Invalid compressed data".to_owned()))?;
            consumed += usize::try_from(self.ctx.total_in() - total_in).unwrap_or(usize::MAX);
            if out.len() > max_size {
                return Err((1009, "Message is too large".to_owned()));
            }
            if status == flate2::Status::StreamEnd {
                // A final block ends the stream: kj resets and stops.
                self.ctx.reset(false);
                break;
            }
            if consumed >= message.len() && out.len() - len < space {
                break;
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;

    use super::*;
    use crate::ffi::WsMessageKind;
    use crate::ws::Role;
    use crate::ws::RustWebSocket;

    // RFC 7692 section 7.2.3.1: "Hello" in one compressed frame.
    const HELLO: [u8; 7] = [0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00];

    fn params(no_context_takeover: bool, window_bits: u8) -> WsCompression {
        WsCompression {
            enabled: true,
            outbound_no_context_takeover: no_context_takeover,
            inbound_no_context_takeover: no_context_takeover,
            outbound_max_window_bits: window_bits,
            inbound_max_window_bits: window_bits,
        }
    }

    /// A server-role socket with deflate, and the raw peer end to write frames into.
    fn server() -> (tokio::io::DuplexStream, RustWebSocket) {
        let (peer, ours) = tokio::io::duplex(1 << 20);
        (
            peer,
            RustWebSocket::new(
                Box::new(ours),
                Role::Server,
                &params(false, 0),
                crate::io::Hangup::never(),
            ),
        )
    }

    #[test]
    fn rfc7692_vectors_with_context_takeover() {
        let mut deflater = Deflater::new(&params(false, 0));
        assert_eq!(deflater.compress(b"Hello").unwrap(), HELLO);
        // Section 7.2.3.2: the same message again refers back to the first.
        assert_eq!(
            deflater.compress(b"Hello").unwrap(),
            [0xf2, 0x00, 0x11, 0x00, 0x00]
        );
    }

    #[test]
    fn no_context_takeover_compresses_each_message_alone() {
        let mut deflater = Deflater::new(&params(true, 0));
        assert_eq!(deflater.compress(b"Hello").unwrap(), HELLO);
        assert_eq!(deflater.compress(b"Hello").unwrap(), HELLO);
    }

    #[test]
    fn empty_message_is_one_empty_block() {
        let mut deflater = Deflater::new(&params(false, 0));
        assert_eq!(deflater.compress(b"").unwrap(), [0x00]);
        let mut inflater = Inflater::new(&params(false, 0));
        assert_eq!(inflater.decompress(vec![0x00], 16).unwrap(), b"");
    }

    #[test]
    fn fragmented_message_with_a_ping_between_fragments() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server();
            // Section 7.2.3.3: "Hello" compressed and split over two frames; a ping in between.
            peer.write_all(&[0x41, 0x03, 0xf2, 0x48, 0xcd])
                .await
                .unwrap();
            peer.write_all(&[0x89, 0x01, b'p']).await.unwrap();
            peer.write_all(&[0x80, 0x04, 0xc9, 0xc9, 0x07, 0x00])
                .await
                .unwrap();
            let message = ws.receive(1 << 20).await.unwrap();
            assert_eq!(message.kind, WsMessageKind::TEXT);
            assert_eq!(message.data, b"Hello");
            let mut pong = [0; 3];
            peer.read_exact(&mut pong).await.unwrap();
            assert_eq!(pong, [0x8a, 0x01, b'p']);
        });
    }

    #[test]
    fn masked_compressed_frame_then_uncompressed_frame() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server();
            let mask = [0x12, 0x34, 0x56, 0x78];
            let mut frame = vec![0xc1, 0x80 | 7];
            frame.extend_from_slice(&mask);
            frame.extend(HELLO.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
            peer.write_all(&frame).await.unwrap();
            peer.write_all(b"\x81\x02hi").await.unwrap();
            assert_eq!(ws.receive(1 << 20).await.unwrap().data, b"Hello");
            assert_eq!(ws.receive(1 << 20).await.unwrap().data, b"hi");
        });
    }

    async fn protocol_error(frames: &[u8], max_size: u64) -> u16 {
        let (mut peer, ws) = server();
        peer.write_all(frames).await.unwrap();
        let message = ws.receive(max_size).await.unwrap();
        assert_eq!(message.kind, WsMessageKind::PROTOCOL_ERROR);
        message.close_code
    }

    #[test]
    fn invalid_compressed_data_is_a_protocol_violation() {
        futures::executor::block_on(async {
            assert_eq!(protocol_error(&[0xc2, 0x01, 0xff], 1 << 20).await, 1002);
        });
    }

    #[test]
    fn rsv2_is_a_protocol_violation() {
        futures::executor::block_on(async {
            assert_eq!(protocol_error(&[0xa1, 0x00], 1 << 20).await, 1002);
        });
    }

    #[test]
    fn decompression_is_bounded_by_max_size() {
        futures::executor::block_on(async {
            let compressed = Deflater::new(&params(false, 0))
                .compress(&vec![0u8; 1 << 20])
                .unwrap();
            let mut frame = vec![0xc2, 126];
            frame.extend_from_slice(&u16::try_from(compressed.len()).unwrap().to_be_bytes());
            frame.extend_from_slice(&compressed);
            assert_eq!(protocol_error(&frame, 1 << 16).await, 1009);
        });
    }

    fn pseudo_random(len: usize, seed: u64) -> Vec<u8> {
        let mut state = seed;
        (0..len)
            .map(|_| {
                state = state
                    .wrapping_mul(6_364_136_223_846_793_005)
                    .wrapping_add(1);
                (state >> 56) as u8
            })
            .collect()
    }

    #[test]
    fn round_trip_across_parameters() {
        futures::executor::block_on(async {
            let compressible = b"workerd permessage-deflate ".repeat(1000);
            let random = pseudo_random(20_000, 7);
            for window_bits in [0, 8, 9, 12, 15] {
                for no_context_takeover in [false, true] {
                    let compression = params(no_context_takeover, window_bits);
                    let (a, b) = tokio::io::duplex(1 << 16);
                    let client = RustWebSocket::new(
                        Box::new(a),
                        Role::Client,
                        &compression,
                        crate::io::Hangup::never(),
                    );
                    let server = RustWebSocket::new(
                        Box::new(b),
                        Role::Server,
                        &compression,
                        crate::io::Hangup::never(),
                    );
                    for round in 0..2 {
                        for (is_text, message) in [
                            (true, &b""[..]),
                            (true, &b"Hello"[..]),
                            (false, &compressible[..]),
                            (false, &random[..]),
                        ] {
                            let (sent, received) = futures::join!(
                                client.send(is_text, message),
                                server.receive(1 << 30)
                            );
                            sent.unwrap();
                            let received = received.unwrap();
                            let kind = if is_text {
                                WsMessageKind::TEXT
                            } else {
                                WsMessageKind::BINARY
                            };
                            assert_eq!(received.kind, kind, "bits={window_bits} round={round}");
                            assert_eq!(received.data, message, "bits={window_bits} round={round}");
                            let (sent, echoed) = futures::join!(
                                server.send(is_text, &received.data),
                                client.receive(1 << 30)
                            );
                            sent.unwrap();
                            assert_eq!(echoed.unwrap().data, message);
                        }
                    }
                }
            }
        });
    }
}
