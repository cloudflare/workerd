//! `kj::WebSocket` over an upgraded connection.
//!
//! Frames are read and written directly on the stream with tungstenite's frame header codec, and
//! messages follow kj's `WebSocketImpl`: the application owns the close handshake, text is opaque
//! bytes, `maxSize` bounds a message before it is buffered, pings are answered, byte counts are
//! wire bytes, and permessage-deflate (`crate::deflate`) applies when it was agreed.

use std::cell::Cell;
use std::cell::RefCell;
use std::io::Cursor;

use bytes::Buf;
use bytes::BytesMut;
use kj::KjError;
use kj::KjExceptionType;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::ReadHalf;
use tokio::io::WriteHalf;
use tokio::sync::watch;
use tokio_tungstenite::tungstenite::protocol::frame::Frame;
use tokio_tungstenite::tungstenite::protocol::frame::FrameHeader;
use tokio_tungstenite::tungstenite::protocol::frame::coding::Control;
use tokio_tungstenite::tungstenite::protocol::frame::coding::Data;
use tokio_tungstenite::tungstenite::protocol::frame::coding::OpCode;

use crate::deflate::Deflater;
use crate::deflate::Inflater;
use crate::ffi::WsCompression;
use crate::ffi::WsMessage;
use crate::ffi::WsMessageKind;
use crate::io::BoxIo;
use crate::io::Hangup;

/// Which end of the connection this is: a client masks what it sends.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Role {
    Client,
    Server,
}

struct Reader {
    io: ReadHalf<BoxIo>,
    buf: BytesMut,
    /// A fragmented message being received: its opcode, whether it is compressed, and its payload.
    partial: Option<(Data, bool, Vec<u8>)>,
    inflater: Option<Inflater>,
}

struct Writer {
    io: WriteHalf<BoxIo>,
    deflater: Option<Deflater>,
}

/// See the module docs.
pub struct RustWebSocket {
    reader: RefCell<Option<Reader>>,
    writer: RefCell<Option<Writer>>,
    role: Role,
    /// A control frame owed to the peer while a send holds the writer: a pong, or the Close for a
    /// protocol error (which replaces a pong, as in kj).
    pending_control: RefCell<Option<(Control, Vec<u8>)>>,
    /// `disconnect()` came while a send held the writer: the send shuts the stream down when done.
    shutdown_pending: Cell<bool>,
    /// The write half after shutdown, kept so the connection stays open (unread input would
    /// otherwise reset it, losing what was last sent) until the WebSocket is destroyed.
    shut_writer: RefCell<Option<Writer>>,
    send_closed: Cell<bool>,
    aborted: Cell<bool>,
    /// Signals `when_aborted` waiters; dropped with the socket, which also wakes them.
    abort_signal: watch::Sender<bool>,
    /// The connection going away also aborts the socket (kj's `whenAborted`).
    hangup: Hangup,
    sent: Cell<u64>,
    received: Cell<u64>,
}

/// A half lent to one operation (kj allows one send and one receive at a time). It goes back when
/// the operation ends or is cancelled, unless the socket was shut in the meantime.
struct Lent<'a, T> {
    slot: &'a RefCell<Option<T>>,
    closed: &'a Cell<bool>,
    value: Option<T>,
}

impl<'a, T> Lent<'a, T> {
    fn take(slot: &'a RefCell<Option<T>>, closed: &'a Cell<bool>) -> Self {
        let value = if closed.get() {
            None
        } else {
            slot.borrow_mut().take()
        };
        Self {
            slot,
            closed,
            value,
        }
    }
}

impl<T> Drop for Lent<'_, T> {
    fn drop(&mut self) {
        if !self.closed.get() {
            *self.slot.borrow_mut() = self.value.take();
        }
    }
}

enum Failure {
    /// Fail the connection with a Close frame carrying this code and reason.
    Protocol(u16, String),
    Io(KjError),
}

fn disconnected(what: &str) -> KjError {
    KjError::new(KjExceptionType::Disconnected, what.to_owned())
}

fn protocol(code: u16, description: impl Into<String>) -> Failure {
    Failure::Protocol(code, description.into())
}

/// A Close frame's payload, as kj serializes it: 1005 means no payload at all.
fn close_payload(code: u16, reason: &[u8]) -> Vec<u8> {
    if code == 1005 {
        return Vec::new();
    }
    let mut payload = Vec::with_capacity(reason.len() + 2);
    payload.extend_from_slice(&code.to_be_bytes());
    payload.extend_from_slice(reason);
    payload
}

impl RustWebSocket {
    pub fn new(io: BoxIo, role: Role, compression: &WsCompression, hangup: Hangup) -> Self {
        let (read, write) = tokio::io::split(io);
        Self {
            reader: RefCell::new(Some(Reader {
                io: read,
                buf: BytesMut::with_capacity(8192),
                partial: None,
                inflater: compression.enabled.then(|| Inflater::new(compression)),
            })),
            writer: RefCell::new(Some(Writer {
                io: write,
                deflater: compression.enabled.then(|| Deflater::new(compression)),
            })),
            role,
            pending_control: RefCell::new(None),
            shutdown_pending: Cell::new(false),
            shut_writer: RefCell::new(None),
            send_closed: Cell::new(false),
            aborted: Cell::new(false),
            abort_signal: watch::channel(false).0,
            hangup,
            sent: Cell::new(0),
            received: Cell::new(0),
        }
    }

    async fn write_frame(
        &self,
        writer: &mut Writer,
        opcode: OpCode,
        payload: Vec<u8>,
        rsv1: bool,
    ) -> kj::Result<()> {
        let mut frame = Frame::message(payload, opcode, true);
        frame.header_mut().rsv1 = rsv1;
        if self.role == Role::Client {
            frame.header_mut().mask = Some(rand::random());
        }
        let mut bytes = Vec::with_capacity(frame.len());
        frame
            .format(&mut bytes)
            .map_err(|e| KjError::new(KjExceptionType::Failed, e.to_string()))?;
        writer
            .io
            .write_all(&bytes)
            .await
            .map_err(|e| crate::io::io_kj_error(&e))?;
        self.sent.set(self.sent.get() + bytes.len() as u64);
        Ok(())
    }

    async fn send_frame(&self, opcode: OpCode, data: &[u8]) -> kj::Result<()> {
        let mut lent = Lent::take(&self.writer, &self.send_closed);
        let Some(writer) = lent.value.as_mut() else {
            return Err(disconnected("WebSocket: can't send after disconnect()"));
        };
        let result = async {
            let (payload, rsv1) = match (writer.deflater.as_mut(), opcode) {
                (Some(deflater), OpCode::Data(_)) => (
                    deflater
                        .compress(data)
                        .map_err(|e| KjError::new(KjExceptionType::Failed, e))?,
                    true,
                ),
                _ => (data.to_vec(), false),
            };
            self.write_frame(writer, opcode, payload, rsv1).await?;
            let control = self.pending_control.borrow_mut().take();
            if let Some((control, payload)) = control {
                self.write_frame(writer, OpCode::Control(control), payload, false)
                    .await?;
            }
            if self.shutdown_pending.get() {
                let _ = writer.io.shutdown().await;
            }
            Ok(())
        }
        .await;
        drop(lent);
        result
    }

    pub async fn send(&self, is_text: bool, data: &[u8]) -> kj::Result<()> {
        let opcode = if is_text { Data::Text } else { Data::Binary };
        self.send_frame(OpCode::Data(opcode), data).await
    }

    pub async fn close(&self, code: u16, reason: &[u8]) -> kj::Result<()> {
        self.send_frame(
            OpCode::Control(Control::Close),
            &close_payload(code, reason),
        )
        .await
    }

    /// Sends a control frame now, or after the send in progress; a pending Close is never
    /// replaced by a pong.
    async fn send_control(&self, control: Control, payload: Vec<u8>) -> kj::Result<()> {
        let mut lent = Lent::take(&self.writer, &self.send_closed);
        if let Some(writer) = lent.value.as_mut() {
            return self
                .write_frame(writer, OpCode::Control(control), payload, false)
                .await;
        }
        let mut pending = self.pending_control.borrow_mut();
        let has_close = matches!(pending.as_ref(), Some((Control::Close, _)));
        if !(self.send_closed.get() || has_close && control == Control::Pong) {
            *pending = Some((control, payload));
        }
        Ok(())
    }

    /// Reads more bytes into the reader's buffer; `eof` names what an end of stream interrupts.
    async fn fill(&self, reader: &mut Reader, eof: &str) -> Result<(), Failure> {
        let n = reader
            .io
            .read_buf(&mut reader.buf)
            .await
            .map_err(|e| Failure::Io(crate::io::io_kj_error(&e)))?;
        if n == 0 {
            let message = if reader.buf.is_empty() && reader.partial.is_none() {
                "WebSocket disconnected between frames without sending `Close`."
            } else {
                eof
            };
            return Err(Failure::Io(disconnected(message)));
        }
        self.received.set(self.received.get() + n as u64);
        Ok(())
    }

    /// One frame, unmasked; its size is checked against `max_size` from the header alone.
    async fn next_frame(
        &self,
        reader: &mut Reader,
        max_size: u64,
    ) -> Result<(FrameHeader, BytesMut), Failure> {
        let (header, length, header_len) = loop {
            let mut cursor = Cursor::new(&reader.buf[..]);
            match FrameHeader::parse(&mut cursor) {
                Ok(Some((header, length))) => {
                    let header_len = usize::try_from(cursor.position()).unwrap_or(usize::MAX);
                    break (header, length, header_len);
                }
                Ok(None) => self.fill(reader, "WebSocket EOF in frame header").await?,
                Err(e) => return Err(protocol(1002, e.to_string())),
            }
        };
        if header.rsv2 || header.rsv3 {
            return Err(protocol(1002, "Received frame had RSV bits 2 or 3 set"));
        }
        if length > max_size {
            return Err(protocol(
                1009,
                format!("Message is too large: {length} > {max_size}"),
            ));
        }
        let length = usize::try_from(length).unwrap_or(usize::MAX);
        while reader.buf.len() < header_len + length {
            self.fill(reader, "WebSocket EOF in message").await?;
        }
        reader.buf.advance(header_len);
        let mut payload = reader.buf.split_to(length);
        if let Some(mask) = header.mask {
            for (i, byte) in payload.iter_mut().enumerate() {
                *byte ^= mask[i & 3];
            }
        }
        Ok((header, payload))
    }

    async fn next_message(&self, reader: &mut Reader, max_size: u64) -> Result<WsMessage, Failure> {
        loop {
            let (header, payload) = self.next_frame(reader, max_size).await?;
            let data = match header.opcode {
                OpCode::Control(control) => {
                    if !header.is_final {
                        return Err(protocol(1002, "Received fragmented control frame"));
                    }
                    match control {
                        Control::Ping => self
                            .send_control(Control::Pong, payload.to_vec())
                            .await
                            .map_err(Failure::Io)?,
                        Control::Pong => {}
                        Control::Close => {
                            let (code, reason) = if payload.len() < 2 {
                                (1005, Vec::new())
                            } else {
                                (
                                    u16::from_be_bytes([payload[0], payload[1]]),
                                    payload[2..].to_vec(),
                                )
                            };
                            return Ok(WsMessage {
                                kind: WsMessageKind::CLOSE,
                                data: reason,
                                close_code: code,
                            });
                        }
                        Control::Reserved(op) => {
                            return Err(protocol(1002, format!("Unknown opcode {op}")));
                        }
                    }
                    continue;
                }
                OpCode::Data(data) => data,
            };

            let (opcode, compressed, mut message) = match (data, reader.partial.take()) {
                (Data::Continue, None) => {
                    return Err(protocol(1002, "Unexpected continuation frame"));
                }
                (Data::Continue, Some(partial)) => partial,
                (_, Some(_)) => return Err(protocol(1002, "Missing continuation frame")),
                (Data::Reserved(op), None) => {
                    return Err(protocol(1002, format!("Unknown opcode {op}")));
                }
                (opcode, None) => {
                    if header.rsv1 && reader.inflater.is_none() {
                        return Err(protocol(
                            1002,
                            "Received a WebSocket frame whose compression bit was set, but the \
                             compression extension was not negotiated for this connection.",
                        ));
                    }
                    (opcode, header.rsv1, Vec::new())
                }
            };
            if (message.len() + payload.len()) as u64 > max_size {
                let total = message.len() + payload.len();
                return Err(protocol(
                    1009,
                    format!("Message is too large: {total} > {max_size}"),
                ));
            }
            message.extend_from_slice(&payload);
            if !header.is_final {
                reader.partial = Some((opcode, compressed, message));
                continue;
            }
            if compressed && let Some(inflater) = reader.inflater.as_mut() {
                message = inflater
                    .decompress(message, usize::try_from(max_size).unwrap_or(usize::MAX))
                    .map_err(|(code, description)| Failure::Protocol(code, description))?;
            }
            let kind = if opcode == Data::Text {
                WsMessageKind::TEXT
            } else {
                WsMessageKind::BINARY
            };
            return Ok(WsMessage {
                kind,
                data: message,
                close_code: 0,
            });
        }
    }

    pub async fn receive(&self, max_size: u64) -> kj::Result<WsMessage> {
        let mut lent = Lent::take(&self.reader, &self.aborted);
        let Some(reader) = lent.value.as_mut() else {
            return Err(disconnected("WebSocket: can't receive after abort()"));
        };
        let result = self.next_message(reader, max_size).await;
        drop(lent);
        match result {
            Ok(message) => Ok(message),
            Err(Failure::Protocol(code, description)) => {
                // As kj: tell the peer why, then report the error to the application.
                let payload = close_payload(code, description.as_bytes());
                let _ = self.send_control(Control::Close, payload).await;
                self.set_aborted();
                Ok(WsMessage {
                    kind: WsMessageKind::PROTOCOL_ERROR,
                    data: description.into_bytes(),
                    close_code: code,
                })
            }
            Err(Failure::Io(error)) => {
                self.set_aborted();
                Err(error)
            }
        }
    }

    /// kj's `disconnect()`: shut the write side down without a Close frame.
    pub async fn disconnect(&self) {
        self.send_closed.set(true);
        let writer = self.writer.borrow_mut().take();
        match writer {
            Some(mut writer) => {
                let _ = writer.io.shutdown().await;
                *self.shut_writer.borrow_mut() = Some(writer);
            }
            None => self.shutdown_pending.set(true),
        }
    }

    /// kj's `abort()`: stop reading and shut the write side down.
    pub async fn abort(&self) {
        self.set_aborted();
        self.reader.borrow_mut().take();
        self.disconnect().await;
    }

    fn set_aborted(&self) {
        self.aborted.set(true);
        self.abort_signal.send_replace(true);
    }

    /// Resolves once the socket is aborted or destroyed, or the peer goes away. It doesn't borrow
    /// the socket, so it may outlive it, as kj's does.
    pub fn when_aborted(&self) -> impl std::future::Future<Output = ()> + 'static {
        let mut aborted = self.abort_signal.subscribe();
        let hangup = self.hangup.clone();
        async move {
            tokio::select! {
                _ = aborted.wait_for(|aborted| *aborted) => {}
                () = hangup => {}
            }
        }
    }

    pub fn sent_byte_count(&self) -> u64 {
        self.sent.get()
    }

    pub fn received_byte_count(&self) -> u64 {
        self.received.get()
    }
}

/// `Sec-WebSocket-Accept` for a `Sec-WebSocket-Key` (RFC 6455).
pub fn accept_key(key: &[u8]) -> String {
    use base64::Engine;
    use sha1::Digest;
    let mut sha = sha1::Sha1::new();
    sha.update(key);
    sha.update(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    base64::engine::general_purpose::STANDARD.encode(sha.finalize())
}
