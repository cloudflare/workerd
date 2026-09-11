//! A `kj::WebSocket`-semantics WebSocket session over a hyper-upgraded connection.
//!
//! # Why hand-rolled framing (rather than tungstenite)
//!
//! The framing layer is written here rather than reusing tokio-tungstenite because kj's observable
//! semantics conflict with tungstenite's protocol automation in several load-bearing ways:
//!
//! - **Close handshake**: tungstenite automatically echoes a received Close frame; kj never sends
//!   a Close the application didn't ask for — workerd's pump loops echo the application-chosen
//!   code, which may differ from the peer's.
//! - **Compression**: workerd runs kj's `MANUAL_COMPRESSION` model — permessage-deflate parameters
//!   are negotiated *by workerd* and handed down pre-agreed. tungstenite's deflate support is tied
//!   to its own handshake/negotiation machinery, but the handshake here belongs to hyper.
//! - **Text validation**: kj does not validate UTF-8 in text messages (workerd depends on this
//!   passthrough); tungstenite rejects invalid UTF-8.
//! - **Limits/errors**: kj has specific close-code + exception behavior on oversized/malformed
//!   frames (send a Close with 1002/1009 and then throw) that must be matched exactly.
//!
//! The protocol state machine below is a port of `kj::WebSocketImpl` (kj/compat/http.c++),
//! including its auto-pong queueing, fragment reassembly, per-message deflate handling (raw
//! deflate, `Z_SYNC_FLUSH`, 4-byte tail strip/append, context-takeover resets, the windowBits=8→9
//! zlib quirk), and its exact error texts.
//!
//! # Concurrency model
//!
//! One `WsSession` is shared by every `kj::WebSocket` method as `&self`; kj allows one `send()`
//! and one `receive()` to be outstanding simultaneously (plus `whenAborted()`), and all of them are
//! polled by the same KJ event loop, so plain `Cell`/`RefCell` state with poll-scoped borrows is
//! sufficient. Like kj, a canceled or failed `send()` poisons the send side (`currentlySending`
//! remains set).

use std::cell::Cell;
use std::cell::RefCell;
use std::rc::Rc;
use std::task::Poll;

use cxx::KjError;
use cxx::KjExceptionType;

use crate::ffi;
use crate::upgraded_io::PulseEvent;
use crate::upgraded_io::SharedIo;
use crate::ws_ext::CompressionConfig;
use crate::ws_ext::generate_extension_request;
use crate::ws_ext::generate_extension_response;

pub const OPCODE_CONTINUATION: u8 = 0;
pub const OPCODE_TEXT: u8 = 1;
pub const OPCODE_BINARY: u8 = 2;
pub const OPCODE_CLOSE: u8 = 8;
pub const OPCODE_PING: u8 = 9;
pub const OPCODE_PONG: u8 = 10;
const OPCODE_FIRST_CONTROL: u8 = 8;

/// Which side of the connection this session is; clients mask their outgoing frames.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Client,
    Server,
}

fn failed(message: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Failed, message.into())
}

fn disconnected(message: impl Into<String>) -> KjError {
    KjError::new(KjExceptionType::Disconnected, message.into())
}

// =======================================================================================
// Frame codec (RFC 6455 wire format)

struct FrameHeader {
    fin: bool,
    rsv1: bool,
    rsv2_or_3: bool,
    opcode: u8,
    mask: Option<[u8; 4]>,
    payload_len: u64,
}

/// Parses a frame header from the front of `buf`. Returns `None` if more bytes are needed.
#[expect(
    clippy::expect_used,
    reason = "the `buf.len() < total` guard above returns None before these fixed-width slices are read, so the `try_into` conversions of the 8-byte length and 4-byte mask always succeed"
)]
fn parse_frame_header(buf: &[u8]) -> Option<(FrameHeader, usize)> {
    if buf.len() < 2 {
        return None;
    }
    let byte0 = buf[0];
    let byte1 = buf[1];
    let has_mask = byte1 & 0x80 != 0;
    let len7 = byte1 & 0x7F;
    let (len_bytes, mut pos) = match len7 {
        126 => (2, 2),
        127 => (8, 2),
        _ => (0, 2),
    };
    let total = 2 + len_bytes + if has_mask { 4 } else { 0 };
    if buf.len() < total {
        return None;
    }
    let payload_len: u64 = match len7 {
        126 => u64::from(u16::from_be_bytes([buf[2], buf[3]])),
        127 => u64::from_be_bytes(buf[2..10].try_into().expect("length checked above")),
        n => u64::from(n),
    };
    pos += len_bytes;
    let mask = if has_mask {
        let key: [u8; 4] = buf[pos..pos + 4].try_into().expect("length checked above");
        pos += 4;
        Some(key)
    } else {
        None
    };
    debug_assert_eq!(pos, total);
    Some((
        FrameHeader {
            fin: byte0 & 0x80 != 0,
            rsv1: byte0 & 0x40 != 0,
            rsv2_or_3: byte0 & 0x30 != 0,
            opcode: byte0 & 0x0F,
            mask,
            payload_len,
        },
        total,
    ))
}

/// Composes a frame header. `mask` of `Some` sets the mask bit and appends the key.
fn compose_frame_header(
    fin: bool,
    compressed: bool,
    opcode: u8,
    payload_len: usize,
    mask: Option<[u8; 4]>,
) -> Vec<u8> {
    let mut header = Vec::with_capacity(14);
    header.push(u8::from(fin) << 7 | u8::from(compressed) << 6 | opcode);
    let mask_bit = if mask.is_some() { 0x80 } else { 0 };
    if payload_len < 126 {
        #[expect(clippy::cast_possible_truncation)]
        header.push(mask_bit | payload_len as u8);
    } else if payload_len <= 0xFFFF {
        header.push(mask_bit | 0x7E);
        #[expect(clippy::cast_possible_truncation)]
        header.extend_from_slice(&(payload_len as u16).to_be_bytes());
    } else {
        header.push(mask_bit | 0x7F);
        header.extend_from_slice(&(payload_len as u64).to_be_bytes());
    }
    if let Some(key) = mask {
        header.extend_from_slice(&key);
    }
    header
}

fn apply_mask(data: &mut [u8], key: [u8; 4]) {
    for (i, byte) in data.iter_mut().enumerate() {
        *byte ^= key[i % 4];
    }
}

// =======================================================================================
// permessage-deflate (mirrors kj's ZlibContext: raw deflate, Z_SYNC_FLUSH per message)

/// A frame-level protocol violation: close the connection with `code` and throw.
struct ProtocolViolation {
    code: u16,
    description: String,
}

/// Applies kj's zlib windowBits quirk: zlib cannot deflate with windowBits 8; 9 is
/// wire-compatible for the receiver (<https://bugs.chromium.org/p/chromium/issues/detail?id=691074>).
/// (flate2 also requires >= 9 for the *inflater*, where kj passes 8 through to zlib; a 9-bit
/// inflate window strictly contains an 8-bit one, so this is receive-compatible.)
fn effective_window_bits(bits: Option<u8>) -> u8 {
    match bits.unwrap_or(15) {
        8 => 9,
        b => b,
    }
}

struct Deflater {
    ctx: flate2::Compress,
    reset_per_message: bool,
}

impl Deflater {
    fn new(config: CompressionConfig) -> Self {
        Self {
            ctx: flate2::Compress::new_with_window_bits(
                flate2::Compression::default(),
                false, // raw deflate (no zlib header), like kj's negative windowBits
                effective_window_bits(config.outbound_max_window_bits),
            ),
            reset_per_message: config.outbound_no_context_takeover,
        }
    }

    /// Compress one message. Returns the frame payload with the trailing `00 00 FF FF` sync
    /// marker stripped (RFC 7692 §7.2.1); an empty message becomes a single empty DEFLATE
    /// block (§7.2.3.6), exactly like kj.
    fn compress_message(&mut self, message: &[u8]) -> Result<Vec<u8>, KjError> {
        if self.reset_per_message {
            self.ctx.reset();
        }
        if message.is_empty() {
            return Ok(vec![0x00]);
        }
        let mut out = Vec::with_capacity(message.len() / 2 + 16);
        let mut buf = vec![0u8; 4096];
        let mut consumed = 0usize;
        loop {
            let before_in = self.ctx.total_in();
            let before_out = self.ctx.total_out();
            self.ctx
                .compress(&message[consumed..], &mut buf, flate2::FlushCompress::Sync)
                .map_err(|e| failed(format!("Error compressing websocket message: {e}")))?;
            #[expect(clippy::cast_possible_truncation)]
            {
                consumed += (self.ctx.total_in() - before_in) as usize;
                let produced = (self.ctx.total_out() - before_out) as usize;
                out.extend_from_slice(&buf[..produced]);
                // Mirror kj's loop condition: done once all input is consumed and the output
                // buffer was not filled to the brim (i.e. zlib has flushed everything).
                if consumed == message.len() && produced < buf.len() {
                    break;
                }
            }
        }
        if !out.ends_with(&[0x00, 0x00, 0xFF, 0xFF]) {
            return Err(failed(
                "Error compressing websocket message: missing sync flush marker".to_owned(),
            ));
        }
        out.truncate(out.len() - 4);
        Ok(out)
    }
}

struct Inflater {
    ctx: flate2::Decompress,
    reset_per_message: bool,
}

impl Inflater {
    fn new(config: CompressionConfig) -> Self {
        Self {
            ctx: flate2::Decompress::new_with_window_bits(
                false, // raw inflate
                effective_window_bits(config.inbound_max_window_bits),
            ),
            reset_per_message: config.inbound_no_context_takeover,
        }
    }

    /// Decompress one message (the caller appends the `00 00 FF FF` tail first, RFC 7692
    /// §7.2.2). `max_size` caps the decompressed size (kj's OOM guard).
    fn decompress_message(
        &mut self,
        message: &[u8],
        max_size: usize,
    ) -> Result<Vec<u8>, ProtocolViolation> {
        if self.reset_per_message {
            self.ctx.reset(false);
        }
        let mut out = Vec::new();
        let mut buf = vec![0u8; 4096];
        let mut consumed = 0usize;
        loop {
            let before_in = self.ctx.total_in();
            let before_out = self.ctx.total_out();
            let status = self
                .ctx
                .decompress(
                    &message[consumed..],
                    &mut buf,
                    flate2::FlushDecompress::Sync,
                )
                .map_err(|_| ProtocolViolation {
                    code: 1002,
                    description: "Invalid compressed data".to_owned(),
                })?;
            #[expect(clippy::cast_possible_truncation)]
            let (used, produced) = (
                (self.ctx.total_in() - before_in) as usize,
                (self.ctx.total_out() - before_out) as usize,
            );
            consumed += used;
            out.extend_from_slice(&buf[..produced]);
            if out.len() > max_size {
                return Err(ProtocolViolation {
                    code: 1009,
                    description: "Message is too large".to_owned(),
                });
            }
            if status == flate2::Status::StreamEnd {
                // A BFINAL block ends the stream; kj resets the context and stops.
                self.ctx.reset(false);
                break;
            }
            if consumed == message.len() && produced < buf.len() {
                break;
            }
        }
        Ok(out)
    }
}

// =======================================================================================
// Session state

struct QueuedControl {
    opcode: u8,
    payload: Vec<u8>,
}

struct PendingWrite {
    frame: Vec<u8>,
    written: usize,
}

struct RecvState {
    buffer: Vec<u8>,
    start: usize,
    end: usize,
    fragments: Vec<Vec<u8>>,
    fragment_opcode: u8,
    fragment_compressed: bool,
}

impl RecvState {
    fn new(leftover: &[u8]) -> Self {
        let mut buffer = vec![0u8; 4096.max(leftover.len())];
        buffer[..leftover.len()].copy_from_slice(leftover);
        Self {
            end: leftover.len(),
            buffer,
            start: 0,
            fragments: Vec::new(),
            fragment_opcode: 0,
            fragment_compressed: false,
        }
    }

    fn window(&self) -> &[u8] {
        &self.buffer[self.start..self.end]
    }

    fn consume(&mut self, n: usize) {
        self.start += n;
        if self.start == self.end {
            self.start = 0;
            self.end = 0;
        }
    }

    /// Moves buffered data to the front so there is room to read more.
    fn compact(&mut self) {
        if self.start > 0 {
            self.buffer.copy_within(self.start..self.end, 0);
            self.end -= self.start;
            self.start = 0;
        }
    }
}

/// The Rust side of a `kj::WebSocket` (wrapped by `RustWebSocket` in hyper-server-ffi.c++).
/// All methods take `&self`; see the module docs for the concurrency model.
pub struct WsSession {
    io: Rc<SharedIo>,
    role: Role,
    compression: Option<CompressionConfig>,
    compressor: RefCell<Option<Deflater>>,
    decompressor: RefCell<Option<Inflater>>,

    // --- Send side (mirrors kj's currentlySending / hasSentClose / queuedControlMessage /
    // sendingControlMessage).
    currently_sending: Cell<bool>,
    has_sent_close: Cell<bool>,
    disconnected: Cell<bool>,
    queued_control: RefCell<Option<QueuedControl>>,
    control_pending: RefCell<Option<PendingWrite>>,
    send_done: PulseEvent,

    // --- Receive side.
    receiving: Cell<bool>,
    read_aborted: Cell<bool>,
    recv: RefCell<RecvState>,

    sent_bytes: Cell<u64>,
    received_bytes: Cell<u64>,

    /// Render protocol errors the way workerd's `JsgifyWebSocketErrors` handler does (the kj
    /// default rendering prefixed with "jsg.Error: ", making them JSG-tunneled typed errors).
    /// Mirrors installing a custom `kj::WebSocketErrorHandler` in kj's settings.
    jsgify_errors: bool,
}

impl WsSession {
    pub(crate) fn new(
        io: Rc<SharedIo>,
        role: Role,
        compression: Option<CompressionConfig>,
        jsgify_errors: bool,
    ) -> Self {
        Self {
            compressor: RefCell::new(compression.map(Deflater::new)),
            decompressor: RefCell::new(compression.map(Inflater::new)),
            io,
            role,
            compression,
            currently_sending: Cell::new(false),
            has_sent_close: Cell::new(false),
            disconnected: Cell::new(false),
            queued_control: RefCell::new(None),
            control_pending: RefCell::new(None),
            send_done: PulseEvent::default(),
            receiving: Cell::new(false),
            read_aborted: Cell::new(false),
            recv: RefCell::new(RecvState::new(&[])),
            sent_bytes: Cell::new(0),
            received_bytes: Cell::new(0),
            jsgify_errors,
        }
    }

    fn gen_mask(&self) -> Option<[u8; 4]> {
        match self.role {
            Role::Server => None,
            Role::Client => {
                let mut key: [u8; 4] = rand::random();
                // An all-zero key would be indistinguishable from "unmasked" on kj's side
                // (kj skips masking for a zero key); avoid the 1-in-2^32 edge.
                if key == [0; 4] {
                    key = [0, 0, 0, 1];
                }
                Some(key)
            }
        }
    }

    // -----------------------------------------------------------------------------------
    // Send path

    /// Corresponds to `kj::WebSocket::send()` (both overloads; `is_text` selects the opcode).
    pub async fn send(&self, is_text: bool, message: &[u8]) -> Result<(), KjError> {
        let opcode = if is_text { OPCODE_TEXT } else { OPCODE_BINARY };
        self.send_frame(opcode, message).await
    }

    /// Corresponds to `kj::WebSocket::close()`.
    pub async fn close(&self, code: u16, reason: &[u8]) -> Result<(), KjError> {
        let payload = serialize_close_payload(code, reason)?;
        self.send_frame(OPCODE_CLOSE, &payload).await
    }

    #[expect(
        clippy::expect_used,
        reason = "the compressor is created iff compression is configured, and this branch is only entered when `self.compression.is_some()`, so `compressor` is always Some here"
    )]
    async fn send_frame(&self, opcode: u8, message: &[u8]) -> Result<(), KjError> {
        if self.disconnected.get() {
            return Err(failed("WebSocket can't send after disconnect()"));
        }
        if self.currently_sending.replace(true) {
            // Mirrors kj, including the poisoning behavior: `currently_sending` is only cleared
            // on success, so a canceled or failed send makes all further sends fail here.
            return Err(failed("another message send is already in progress"));
        }

        // A control message (pong / error close) may be queued or partially written; finish it
        // before our frame, like kj's `co_await sendingControlMessage`.
        self.flush_control().await?;
        if self.disconnected.get() {
            return Err(failed("WebSocket can't send after disconnect()"));
        }

        // We don't stop the application from sending after close() (kj doesn't either), but we
        // must not send any queued PONGs after a close.
        if opcode == OPCODE_CLOSE {
            self.has_sent_close.set(true);
        }

        let mut compressed = false;
        let mut payload: Vec<u8>;
        if (opcode == OPCODE_TEXT || opcode == OPCODE_BINARY) && self.compression.is_some() {
            let mut compressor = self.compressor.borrow_mut();
            payload = compressor
                .as_mut()
                .expect("compressor exists when compression is configured")
                .compress_message(message)?;
            compressed = true;
        } else {
            payload = message.to_vec();
        }

        let mask = self.gen_mask();
        if let Some(key) = mask {
            apply_mask(&mut payload, key);
        }
        let mut frame = compose_frame_header(true, compressed, opcode, payload.len(), mask);
        frame.extend_from_slice(&payload);

        self.io.write_all(&frame).await?;
        self.sent_bytes
            .set(self.sent_bytes.get() + frame.len() as u64);
        self.currently_sending.set(false);
        self.send_done.pulse();

        // Write any control message that got queued while we were sending. kj does this in the
        // background; write errors here surface on the next operation that touches the socket.
        let _ = self.flush_control().await;
        Ok(())
    }

    /// Corresponds to `kj::WebSocket::disconnect()`.
    pub fn disconnect(&self) {
        // If we're sending a control message (e.g. a PONG), cancel it (kj does the same).
        *self.queued_control.borrow_mut() = None;
        *self.control_pending.borrow_mut() = None;
        self.disconnected.set(true);
        self.io.shutdown_write();
    }

    /// Corresponds to `kj::WebSocket::abort()`.
    pub fn abort(&self) {
        *self.queued_control.borrow_mut() = None;
        *self.control_pending.borrow_mut() = None;
        self.disconnected.set(true);
        self.read_aborted.set(true);
        self.io.abort();
    }

    /// Corresponds to `kj::WebSocket::whenAborted()`. See `SharedIo::aborted` for the (small)
    /// detection divergence from kj.
    pub async fn when_aborted(&self) {
        self.io.aborted().wait().await;
    }

    #[must_use]
    pub fn sent_byte_count(&self) -> u64 {
        self.sent_bytes.get()
    }

    #[must_use]
    pub fn received_byte_count(&self) -> u64 {
        self.received_bytes.get()
    }

    /// Corresponds to `kj::WebSocket::getPreferredExtensions()`; mirrors kj's client/server ×
    /// request/response matrix exactly (including the confusing request/response swap).
    #[must_use]
    pub fn get_preferred_extensions(&self, request_context: bool) -> ffi::WsPreferredExtensions {
        match self.preferred_extensions(request_context) {
            Some(value) => ffi::WsPreferredExtensions {
                is_some: true,
                value,
            },
            None => ffi::WsPreferredExtensions {
                is_some: false,
                value: String::new(),
            },
        }
    }

    #[must_use]
    fn preferred_extensions(&self, request_context: bool) -> Option<String> {
        match (self.role, request_context) {
            // Server side asked for request headers (proxy pass-through): render our config in
            // response format (maps inbound/outbound to client/server correctly).
            (Role::Server, true) => Some(
                self.compression
                    .map(generate_extension_response)
                    .unwrap_or_default(),
            ),
            // Client side asked for response headers: render our config in request format.
            (Role::Client, false) => Some(
                self.compression
                    .map(generate_extension_request)
                    .unwrap_or_default(),
            ),
            // Pumping between two same-side sockets can't be optimized (masking differs).
            _ => None,
        }
    }

    // -----------------------------------------------------------------------------------
    // Control-message queue (auto-pong, error closes) — mirrors kj's queuedControlMessage

    fn queue_pong(&self, payload: Vec<u8>) {
        let mut queued = self.queued_control.borrow_mut();
        if let Some(control) = &*queued
            && control.opcode == OPCODE_CLOSE
        {
            // We're closing due to an error; a Pong would never be sent anyway.
            return;
        }
        // A newer ping supersedes any queued pong (kj: reply only to the most recent ping).
        *queued = Some(QueuedControl {
            opcode: OPCODE_PONG,
            payload,
        });
    }

    #[expect(
        clippy::expect_used,
        reason = "queue_close is only called with internally-generated close codes, never the reserved 1005 that serialize_close_payload rejects"
    )]
    fn queue_close(&self, code: u16, description: &str) {
        // An error close supersedes any queued pong.
        *self.queued_control.borrow_mut() = Some(QueuedControl {
            opcode: OPCODE_CLOSE,
            payload: serialize_close_payload(code, description.as_bytes())
                .expect("error close codes never use 1005"),
        });
    }

    /// Writes the partially-written control frame (if any), then the queued control message
    /// (if any). Must not run concurrently with a data-frame write; callers coordinate through
    /// `currently_sending`.
    async fn flush_control(&self) -> Result<(), KjError> {
        loop {
            // Stage the queued message as pending bytes if nothing is partially written.
            {
                let mut pending = self.control_pending.borrow_mut();
                if pending.is_none() {
                    let Some(control) = self.queued_control.borrow_mut().take() else {
                        return Ok(());
                    };
                    if self.has_sent_close.get() || self.disconnected.get() {
                        // Like kj's writeQueuedControlMessage: drop it silently.
                        continue;
                    }
                    let mut payload = control.payload;
                    let mask = self.gen_mask();
                    if let Some(key) = mask {
                        apply_mask(&mut payload, key);
                    }
                    let mut frame =
                        compose_frame_header(true, false, control.opcode, payload.len(), mask);
                    frame.extend_from_slice(&payload);
                    *pending = Some(PendingWrite { frame, written: 0 });
                }
            }
            // Write the pending frame to completion. The write progress lives in `self`, so a
            // canceled caller resumes cleanly on the next flush (kj keeps the equivalent
            // in-flight promise in `sendingControlMessage`).
            std::future::poll_fn(|cx| {
                let mut pending_slot = self.control_pending.borrow_mut();
                let Some(pending) = &mut *pending_slot else {
                    return Poll::Ready(Ok(()));
                };
                while pending.written < pending.frame.len() {
                    match self
                        .io
                        .poll_write_some(cx, &pending.frame[pending.written..])
                    {
                        Poll::Pending => return Poll::Pending,
                        Poll::Ready(Ok(n)) => pending.written += n,
                        Poll::Ready(Err(e)) => return Poll::Ready(Err(e)),
                    }
                }
                // Note: kj does not count control frames in sentByteCount(); neither do we.
                *pending_slot = None;
                Poll::Ready(Ok(()))
            })
            .await?;
        }
    }

    /// Control flush from the receive path: skipped while a `send()` is in flight (that send's
    /// completion flushes the queue). Write errors are swallowed, mirroring kj, where they
    /// surface on the next `send()`.
    async fn flush_control_from_receive(&self) {
        if self.currently_sending.get() {
            return;
        }
        let _ = self.flush_control().await;
    }

    // -----------------------------------------------------------------------------------
    // Receive path

    /// Corresponds to `kj::WebSocket::receive(maxSize)`.
    pub async fn receive(&self, max_size: usize) -> Result<ffi::WsMessage, KjError> {
        if self.read_aborted.get() {
            return Err(disconnected("the WebSocket was aborted"));
        }
        if self.receiving.replace(true) {
            return Err(failed("can only call receive() once at a time"));
        }
        let guard = ReceivingGuard(self);
        let result = self.receive_impl(max_size).await;
        drop(guard);
        result
    }

    // Deliberately one long loop: this is a line-for-line port of kj's WebSocketImpl::receive()
    // state machine, and keeping the shape identical makes divergences easy to audit.
    #[expect(clippy::too_many_lines)]
    #[expect(
        clippy::expect_used,
        reason = "the decompressor is created iff compression is configured, and this decompress branch is only reached when compression negotiated it, so `decompressor` is always Some here"
    )]
    async fn receive_impl(&self, max_size: usize) -> Result<ffi::WsMessage, KjError> {
        let mut remaining_max = max_size;
        loop {
            let header = self.read_frame_header().await?;
            if header.rsv2_or_3 {
                return self
                    .fail_protocol(1002, "Received frame had RSV bits 2 or 3 set".to_owned())
                    .await;
            }
            if header.payload_len > remaining_max as u64 {
                return self
                    .fail_protocol(
                        1009,
                        format!(
                            "Message is too large: {} > {remaining_max}",
                            header.payload_len
                        ),
                    )
                    .await;
            }
            #[expect(clippy::cast_possible_truncation)]
            let payload_len = header.payload_len as usize;

            let is_data = header.opcode < OPCODE_FIRST_CONTROL;
            // Continuation/fragment consistency checks (borrow ends before any await).
            let opcode_or_error: Result<u8, &'static str> = {
                let recv = self.recv.borrow();
                if header.opcode == OPCODE_CONTINUATION {
                    if recv.fragments.is_empty() {
                        Err("Unexpected continuation frame")
                    } else {
                        Ok(recv.fragment_opcode)
                    }
                } else if is_data && !recv.fragments.is_empty() {
                    Err("Missing continuation frame")
                } else {
                    Ok(header.opcode)
                }
            };
            let opcode = match opcode_or_error {
                Ok(opcode) => opcode,
                Err(message) => return self.fail_protocol(1002, message.to_owned()).await,
            };

            let mut payload = self.read_payload(payload_len).await?;
            if let Some(key) = header.mask {
                apply_mask(&mut payload, key);
            }

            if !header.fin {
                if !is_data {
                    return self
                        .fail_protocol(1002, "Received fragmented control frame".to_owned())
                        .await;
                }
                let mut recv = self.recv.borrow_mut();
                if recv.fragments.is_empty() {
                    recv.fragment_opcode = header.opcode;
                    recv.fragment_compressed = header.rsv1;
                }
                remaining_max -= payload.len();
                recv.fragments.push(payload);
                continue;
            }

            // Final frame of a message.
            let compressed = if is_data {
                let recv = self.recv.borrow();
                header.rsv1 || (!recv.fragments.is_empty() && recv.fragment_compressed)
            } else {
                false
            };

            match opcode {
                OPCODE_TEXT | OPCODE_BINARY => {
                    // Gather fragments (if any) + this payload.
                    let (mut message, fragments_size) = {
                        let mut recv = self.recv.borrow_mut();
                        let fragments_size: usize =
                            recv.fragments.iter().map(Vec::len).sum::<usize>();
                        let mut message = Vec::with_capacity(fragments_size + payload.len() + 4);
                        for fragment in recv.fragments.drain(..) {
                            message.extend_from_slice(&fragment);
                        }
                        recv.fragment_opcode = 0;
                        recv.fragment_compressed = false;
                        message.extend_from_slice(&payload);
                        (message, fragments_size)
                    };

                    if compressed {
                        if self.compression.is_none() {
                            return self
                                .fail_protocol(
                                    1002,
                                    "Received a WebSocket frame whose compression bit was set, \
                                     but the compression extension was not negotiated for this \
                                     connection."
                                        .to_owned(),
                                )
                                .await;
                        }
                        // Append 00 00 FF FF before inflating (RFC 7692 §7.2.2). The size cap
                        // is the *original* maxSize (kj: fragments already consumed part of it).
                        message.extend_from_slice(&[0x00, 0x00, 0xFF, 0xFF]);
                        let original_max = fragments_size + remaining_max;
                        let decompressed = {
                            let mut decompressor = self.decompressor.borrow_mut();
                            decompressor
                                .as_mut()
                                .expect("decompressor exists when compression is configured")
                                .decompress_message(&message, original_max)
                        };
                        match decompressed {
                            Ok(data) => message = data,
                            Err(violation) => {
                                return self
                                    .fail_protocol(violation.code, violation.description)
                                    .await;
                            }
                        }
                    }

                    return Ok(ffi::WsMessage {
                        kind: if opcode == OPCODE_TEXT {
                            ffi::WsMessageKind::TEXT
                        } else {
                            ffi::WsMessageKind::BINARY
                        },
                        data: message,
                        close_code: 0,
                    });
                }
                OPCODE_CLOSE => {
                    return Ok(if payload.len() < 2 {
                        ffi::WsMessage {
                            kind: ffi::WsMessageKind::CLOSE,
                            data: Vec::new(),
                            close_code: 1005,
                        }
                    } else {
                        let close_code = u16::from_be_bytes([payload[0], payload[1]]);
                        ffi::WsMessage {
                            kind: ffi::WsMessageKind::CLOSE,
                            data: payload[2..].to_vec(),
                            close_code,
                        }
                    });
                }
                OPCODE_PING => {
                    // Auto-pong, echoing the payload (kj replies only to the latest ping).
                    self.queue_pong(payload);
                    self.flush_control_from_receive().await;
                }
                OPCODE_PONG => {
                    // Unsolicited pong. Ignore.
                }
                unknown => {
                    return self
                        .fail_protocol(1002, format!("Unknown opcode {unknown}"))
                        .await;
                }
            }
        }
    }

    /// Reads (buffering) until a complete frame header is available, then consumes and returns
    /// it. EOF texts mirror kj's exactly.
    async fn read_frame_header(&self) -> Result<FrameHeader, KjError> {
        loop {
            {
                let mut recv = self.recv.borrow_mut();
                if let Some((header, size)) = parse_frame_header(recv.window()) {
                    recv.consume(size);
                    return Ok(header);
                }
            }
            let n = std::future::poll_fn(|cx| {
                if self.read_aborted.get() {
                    return Poll::Ready(Err(disconnected("the WebSocket was aborted")));
                }
                let mut recv = self.recv.borrow_mut();
                recv.compact();
                let end = recv.end;
                let RecvState { buffer, .. } = &mut *recv;
                self.io.poll_read_some(cx, &mut buffer[end..])
            })
            .await?;
            if n == 0 {
                let had_partial = !self.recv.borrow().window().is_empty();
                // The peer went away without a Close message; this also counts as an abort for
                // whenAborted() purposes (see SharedIo::aborted docs).
                self.io.aborted().fire();
                return Err(disconnected(if had_partial {
                    "WebSocket EOF in frame header"
                } else {
                    "WebSocket disconnected between frames without sending `Close`."
                }));
            }
            self.recv.borrow_mut().end += n;
            self.received_bytes
                .set(self.received_bytes.get() + n as u64);
        }
    }

    /// Reads exactly `len` payload bytes (first from the buffer, then from the socket).
    async fn read_payload(&self, len: usize) -> Result<Vec<u8>, KjError> {
        let mut payload = vec![0u8; len];
        let mut filled = {
            let mut recv = self.recv.borrow_mut();
            let available = recv.window().len().min(len);
            payload[..available].copy_from_slice(&recv.window()[..available]);
            recv.consume(available);
            available
        };
        while filled < len {
            let n = std::future::poll_fn(|cx| {
                if self.read_aborted.get() {
                    return Poll::Ready(Err(disconnected("the WebSocket was aborted")));
                }
                self.io.poll_read_some(cx, &mut payload[filled..])
            })
            .await?;
            if n == 0 {
                self.io.aborted().fire();
                return Err(disconnected("WebSocket EOF in message"));
            }
            filled += n;
            self.received_bytes
                .set(self.received_bytes.get() + n as u64);
        }
        Ok(payload)
    }

    /// Mirrors kj's `sendCloseDueToError`: queue a Close(code, description) to the peer, wait
    /// for it to be written (waiting out any in-flight `send()` first), then throw the way kj's
    /// default `WebSocketErrorHandler::handleWebSocketProtocolError` does.
    async fn fail_protocol(
        &self,
        code: u16,
        description: String,
    ) -> Result<ffi::WsMessage, KjError> {
        self.queue_close(code, &description);
        while self.currently_sending.get() {
            self.send_done.next_pulse().await;
        }
        // If writing the close fails the protocol error below still wins (kj behaves the same
        // modulo exception ordering).
        let _ = self.flush_control().await;
        // Exactly kj's default `WebSocketErrorHandler::handleWebSocketProtocolError` rendering
        // (KJ_EXCEPTION parameter formatting); with `jsgify_errors`, additionally prefixed the
        // way workerd's JsgifyWebSocketErrors handler wraps it.
        let base = format!(
            "WebSocket protocol error; protocolError.statusCode = {code}; \
             protocolError.description = {description}"
        );
        Err(failed(if self.jsgify_errors {
            format!("jsg.Error: {base}")
        } else {
            base
        }))
    }
}

/// Clears the `receiving` flag when a `receive()` completes or is canceled.
struct ReceivingGuard<'a>(&'a WsSession);

impl Drop for ReceivingGuard<'_> {
    fn drop(&mut self) {
        self.0.receiving.set(false);
    }
}

/// Close-frame payload; mirrors kj's `serializeClose` (1005 means "no code on the wire" and
/// cannot carry a reason).
fn serialize_close_payload(code: u16, reason: &[u8]) -> Result<Vec<u8>, KjError> {
    if code == 1005 {
        if !reason.is_empty() {
            return Err(failed("WebSocket close code 1005 cannot have a reason"));
        }
        return Ok(Vec::new());
    }
    let mut payload = Vec::with_capacity(reason.len() + 2);
    payload.extend_from_slice(&code.to_be_bytes());
    payload.extend_from_slice(reason);
    Ok(payload)
}

// =======================================================================================
// Handshake helpers

/// Computes the `Sec-WebSocket-Accept` value for a `Sec-WebSocket-Key` (RFC 6455 §4.2.2).
#[must_use]
pub fn websocket_accept_key(key: &[u8]) -> String {
    use base64::Engine;
    use sha1::Digest;
    const WEBSOCKET_GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    let mut hasher = sha1::Sha1::new();
    hasher.update(key);
    hasher.update(WEBSOCKET_GUID);
    base64::engine::general_purpose::STANDARD.encode(hasher.finalize())
}

/// Generates a random `Sec-WebSocket-Key` (16 random bytes, base64).
#[must_use]
pub fn generate_websocket_key() -> String {
    use base64::Engine;
    let bytes: [u8; 16] = rand::random();
    base64::engine::general_purpose::STANDARD.encode(bytes)
}
