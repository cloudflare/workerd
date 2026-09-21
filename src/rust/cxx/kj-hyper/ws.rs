//! `kj::WebSocket` over an upgraded connection.
//!
//! Frames are read and written directly on the stream with tungstenite's frame header codec, and
//! messages follow kj's `WebSocketImpl`: the application owns the close handshake, text is opaque
//! bytes, `maxSize` bounds a message before it is buffered, pings are answered, byte counts are
//! wire bytes, and permessage-deflate (`crate::deflate`) applies when it was agreed. (tungstenite's
//! own protocol state machine answers Close and validates text itself, which workerd's WebSocket
//! API, owning both, cannot use.)
//!
//! One operation writes at a time: the writer is lent to an application send, or to the receive
//! answering a ping or a protocol error. A control frame owed while another operation holds the
//! writer, and `disconnect()`'s shutdown, are written by that operation before it gives the writer
//! back ([`WsState::write_owed`]), as kj chains them onto the send in flight.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::io::Cursor;
use std::rc::Rc;

use bytes::Buf;
use bytes::BytesMut;
use cxx::KjError;
use cxx::KjExceptionType;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::io::ReadHalf;
use tokio::io::WriteHalf;
use tokio::sync::Notify;
use tokio::sync::watch;
use tungstenite::protocol::frame::Frame;
use tungstenite::protocol::frame::FrameHeader;
use tungstenite::protocol::frame::coding::Control;
use tungstenite::protocol::frame::coding::Data;
use tungstenite::protocol::frame::coding::OpCode;

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

struct WsState {
    reader: RefCell<Option<Reader>>,
    /// The writer, when no operation holds it. It stays after `disconnect()` shut it down, so the
    /// connection stays open (unread input would otherwise reset it, losing what was last sent).
    writer: RefCell<Option<Writer>>,
    /// Signals operations waiting for the writer that it came back, or that sending ended.
    writer_returned: Notify,
    role: Role,
    /// A control frame owed to the peer: a pong, or the Close for a protocol error (which replaces
    /// a pong, as in kj).
    owed_control: RefCell<Option<(Control, Vec<u8>)>>,
    /// `disconnect()` came while an operation held the writer.
    owed_shutdown: Cell<bool>,
    send_closed: Cell<bool>,
    /// A send ended mid-frame (cancelled or failed), so the stream is past use for sending.
    send_broken: Cell<bool>,
    /// Receiving ended: the socket was aborted, or its receive failed.
    aborted: Cell<bool>,
    /// Signals `when_aborted` waiters; dropped with the socket, which also wakes them.
    abort_signal: watch::Sender<bool>,
    /// `abort()` was called: operations in flight fail, and the connection is let go.
    cancel: watch::Sender<bool>,
    /// The connection going away also aborts the socket (kj's `whenAborted`).
    hangup: Hangup,
    sent: Cell<u64>,
    received: Cell<u64>,
}

/// See the module docs. Each operation owns a share of the socket, so it may outlive the handle.
pub struct RustWebSocket(Rc<WsState>);

/// The writer, lent to one operation. It goes back when the operation finishes; one that ends
/// mid-frame breaks sending for good.
struct LentWriter<'a> {
    ws: &'a WsState,
    writer: Option<Writer>,
    complete: bool,
}

impl Drop for LentWriter<'_> {
    fn drop(&mut self) {
        let ws = self.ws;
        let Some(writer) = self.writer.take() else {
            return;
        };
        if *ws.cancel.borrow() {
            return;
        }
        if self.complete {
            *ws.writer.borrow_mut() = Some(writer);
        } else {
            ws.send_broken.set(true);
            ws.send_closed.set(true);
        }
        ws.writer_returned.notify_waiters();
    }
}

/// The reader, lent to one receive. It goes back when the receive ends, even cancelled (frames are
/// consumed from its buffer only once complete), unless the socket was aborted.
struct LentReader<'a> {
    ws: &'a WsState,
    reader: Option<Reader>,
}

impl Drop for LentReader<'_> {
    fn drop(&mut self) {
        if !self.ws.aborted.get() {
            *self.ws.reader.borrow_mut() = self.reader.take();
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
        Self(Rc::new(WsState {
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
            writer_returned: Notify::new(),
            role,
            owed_control: RefCell::new(None),
            owed_shutdown: Cell::new(false),
            send_closed: Cell::new(false),
            send_broken: Cell::new(false),
            aborted: Cell::new(false),
            abort_signal: watch::channel(false).0,
            cancel: watch::channel(false).0,
            hangup,
            sent: Cell::new(0),
            received: Cell::new(0),
        }))
    }

    pub fn send<'b>(
        &self,
        is_text: bool,
        data: &'b [u8],
    ) -> impl Future<Output = crate::Result<()>> + use<'b> {
        let ws = self.0.clone();
        let opcode = if is_text { Data::Text } else { Data::Binary };
        async move { ws.send_frame(OpCode::Data(opcode), data).await }
    }

    pub fn close<'b>(
        &self,
        code: u16,
        reason: &'b [u8],
    ) -> impl Future<Output = crate::Result<()>> + use<'b> {
        let ws = self.0.clone();
        async move {
            ws.send_frame(
                OpCode::Control(Control::Close),
                &close_payload(code, reason),
            )
            .await
        }
    }

    pub fn receive(&self, max_size: u64) -> impl Future<Output = crate::Result<WsMessage>> + use<> {
        let ws = self.0.clone();
        async move { ws.receive(max_size).await }
    }

    /// kj's `disconnect()`: shut the write side down without a Close frame.
    pub fn disconnect(&self) -> impl Future<Output = ()> + use<> {
        let ws = self.0.clone();
        async move { ws.disconnect().await }
    }

    /// kj's `abort()`: fail the operations in flight and let go of the connection.
    pub fn abort(&self) {
        let ws = &self.0;
        ws.cancel.send_replace(true);
        ws.set_aborted();
        ws.send_closed.set(true);
        ws.writer_returned.notify_waiters();
        ws.reader.borrow_mut().take();
        ws.writer.borrow_mut().take();
    }

    /// Resolves once the socket is aborted or destroyed, or the peer goes away (failing if
    /// observing that fails), as kj's does.
    pub fn when_aborted(&self) -> impl Future<Output = crate::Result<()>> + use<> {
        let mut aborted = self.0.abort_signal.subscribe();
        let hangup = self.0.hangup.clone();
        async move {
            let aborted = std::pin::pin!(async move {
                let _ = aborted.wait_for(|aborted| *aborted).await;
            });
            match futures::future::select(aborted, hangup).await {
                futures::future::Either::Left(((), _)) => Ok(()),
                futures::future::Either::Right((result, _)) => result,
            }
        }
    }

    pub fn sent_byte_count(&self) -> u64 {
        self.0.sent.get()
    }

    pub fn received_byte_count(&self) -> u64 {
        self.0.received.get()
    }
}

impl WsState {
    /// `op`, failing with DISCONNECTED once `abort()` is called.
    async fn unless_aborted<T>(
        &self,
        op: impl Future<Output = crate::Result<T>>,
    ) -> crate::Result<T> {
        let mut cancel = self.cancel.subscribe();
        let cancelled = std::pin::pin!(async move {
            let _ = cancel.wait_for(|cancelled| *cancelled).await;
        });
        let op = std::pin::pin!(op);
        match futures::future::select(op, cancelled).await {
            futures::future::Either::Left((result, _)) => result,
            futures::future::Either::Right(((), _)) => Err(disconnected("WebSocket: aborted")),
        }
    }

    fn send_refusal(&self) -> KjError {
        if self.send_broken.get() {
            disconnected("WebSocket: a previous send ended mid-message")
        } else if *self.cancel.borrow() {
            disconnected("WebSocket: can't send after abort()")
        } else {
            disconnected("WebSocket: can't send after disconnect()")
        }
    }

    /// Waits for the writer while another operation holds it.
    async fn lend_writer(&self) -> crate::Result<LentWriter<'_>> {
        loop {
            if self.send_closed.get() {
                return Err(self.send_refusal());
            }
            let writer = self.writer.borrow_mut().take();
            if let Some(writer) = writer {
                return Ok(LentWriter {
                    ws: self,
                    writer: Some(writer),
                    complete: false,
                });
            }
            self.writer_returned.notified().await;
        }
    }

    async fn write_frame(
        &self,
        writer: &mut Writer,
        opcode: OpCode,
        payload: Vec<u8>,
        rsv1: bool,
    ) -> crate::Result<()> {
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
        // A TLS stream may hold what it was written until flushed.
        writer
            .io
            .flush()
            .await
            .map_err(|e| crate::io::io_kj_error(&e))?;
        self.sent.set(self.sent.get() + bytes.len() as u64);
        Ok(())
    }

    /// Writes what the peer is owed -- control frames, then the shutdown -- before the writer goes
    /// back. Whatever is owed while this runs is written by this loop too.
    async fn write_owed(&self, writer: &mut Writer) -> crate::Result<()> {
        loop {
            let owed = self.owed_control.borrow_mut().take();
            if let Some((control, payload)) = owed {
                self.write_frame(writer, OpCode::Control(control), payload, false)
                    .await?;
            } else if self.owed_shutdown.replace(false) {
                let _ = writer.io.shutdown().await;
            } else {
                return Ok(());
            }
        }
    }

    async fn send_frame(&self, opcode: OpCode, data: &[u8]) -> crate::Result<()> {
        self.unless_aborted(async {
            let mut lent = self.lend_writer().await?;
            let writer = lent.writer.as_mut().ok_or_else(|| self.send_refusal())?;
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
            self.write_owed(writer).await?;
            lent.complete = true;
            Ok(())
        })
        .await
    }

    /// Sends a control frame now, or has the operation holding the writer send it; a pending
    /// Close is never replaced by a pong.
    async fn send_control(&self, control: Control, payload: Vec<u8>) -> crate::Result<()> {
        if self.send_closed.get() {
            return Ok(());
        }
        {
            let mut owed = self.owed_control.borrow_mut();
            let has_close = matches!(owed.as_ref(), Some((Control::Close, _)));
            if !(has_close && control == Control::Pong) {
                *owed = Some((control, payload));
            }
        }
        let writer = self.writer.borrow_mut().take();
        let Some(writer) = writer else {
            return Ok(());
        };
        let mut lent = LentWriter {
            ws: self,
            writer: Some(writer),
            complete: false,
        };
        if let Some(writer) = lent.writer.as_mut() {
            self.write_owed(writer).await?;
        }
        lent.complete = true;
        Ok(())
    }

    async fn disconnect(&self) {
        self.send_closed.set(true);
        self.writer_returned.notify_waiters();
        let writer = self.writer.borrow_mut().take();
        let Some(writer) = writer else {
            // The operation holding the writer shuts it down when done.
            self.owed_shutdown.set(true);
            return;
        };
        let mut lent = LentWriter {
            ws: self,
            writer: Some(writer),
            complete: false,
        };
        if let Some(writer) = lent.writer.as_mut() {
            let _ = writer.io.shutdown().await;
        }
        lent.complete = true;
    }

    fn set_aborted(&self) {
        self.aborted.set(true);
        self.abort_signal.send_replace(true);
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

    /// One frame, unmasked. Its size is checked from the header alone: a control frame against
    /// `max_size`, a data frame against `budget`, what is left of `max_size` for its message.
    async fn next_frame(
        &self,
        reader: &mut Reader,
        max_size: u64,
        budget: u64,
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
        let limit = if matches!(header.opcode, OpCode::Control(_)) {
            max_size
        } else {
            budget
        };
        if length > limit {
            let buffered = max_size - budget;
            return Err(protocol(
                1009,
                format!("Message is too large: {} > {max_size}", buffered + length),
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
            let buffered = reader
                .partial
                .as_ref()
                .map_or(0, |(_, _, m)| m.len() as u64);
            let budget = max_size.saturating_sub(buffered);
            let (header, payload) = self.next_frame(reader, max_size, budget).await?;
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

    async fn receive(&self, max_size: u64) -> crate::Result<WsMessage> {
        self.unless_aborted(async {
            if self.aborted.get() {
                return Err(disconnected("WebSocket: can't receive after abort()"));
            }
            let reader = self.reader.borrow_mut().take();
            let mut lent = LentReader {
                ws: self,
                reader: Some(reader.ok_or_else(|| {
                    KjError::new(
                        KjExceptionType::Failed,
                        "WebSocket: another receive is already in progress".to_owned(),
                    )
                })?),
            };
            let result = match lent.reader.as_mut() {
                Some(reader) => self.next_message(reader, max_size).await,
                None => return Err(disconnected("WebSocket: can't receive after abort()")),
            };
            drop(lent);
            match result {
                Ok(message) => Ok(message),
                Err(Failure::Protocol(code, description)) => {
                    // As kj: tell the peer why, then report the error to the application.
                    self.set_aborted();
                    let payload = close_payload(code, description.as_bytes());
                    let _ = self.send_control(Control::Close, payload).await;
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
        })
        .await
    }
}

/// `Sec-WebSocket-Accept` for a `Sec-WebSocket-Key` (RFC 6455).
pub fn accept_key(key: &[u8]) -> String {
    tungstenite::handshake::derive_accept_key(key)
}

#[cfg(test)]
mod tests {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;

    use super::*;

    fn uncompressed() -> WsCompression {
        WsCompression {
            enabled: false,
            outbound_no_context_takeover: false,
            inbound_no_context_takeover: false,
            outbound_max_window_bits: 0,
            inbound_max_window_bits: 0,
        }
    }

    /// A server-role socket over a pipe holding `capacity` bytes, and the raw peer end.
    fn server(capacity: usize) -> (tokio::io::DuplexStream, RustWebSocket) {
        let (peer, ours) = tokio::io::duplex(capacity);
        let ws = RustWebSocket::new(
            Box::new(ours),
            Role::Server,
            &uncompressed(),
            Hangup::never(),
        );
        (peer, ws)
    }

    #[test]
    fn a_send_waits_while_a_pong_holds_the_writer() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server(16);
            // Fill the pipe toward the peer, so answering a ping waits on the peer.
            ws.send(false, &[b'x'; 14]).await.unwrap();
            let mut ping = vec![0x89, 14];
            ping.extend_from_slice(&[b'p'; 14]);
            peer.write_all(&ping).await.unwrap();
            let mut receive = Box::pin(ws.receive(1 << 20));
            assert!(futures::poll!(receive.as_mut()).is_pending());
            let mut send = Box::pin(ws.send(true, b"hi"));
            assert!(futures::poll!(send.as_mut()).is_pending());
            let mut wire = vec![0; 16 + 16 + 4];
            // The receive keeps running (writing the pong) until the send and the read are done.
            let done = futures::future::join(send, peer.read_exact(&mut wire));
            let futures::future::Either::Right(((send_result, read_result), _)) =
                futures::future::select(receive, Box::pin(done)).await
            else {
                panic!("the receive ended");
            };
            send_result.unwrap();
            read_result.unwrap();
            assert_eq!(&wire[16..18], &[0x8a, 14]);
            assert_eq!(&wire[32..], &[0x81, 2, b'h', b'i']);
        });
    }

    #[test]
    fn abort_fails_a_pending_receive_and_lets_go_of_the_connection() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server(1 << 16);
            let mut receive = Box::pin(ws.receive(1 << 20));
            assert!(futures::poll!(receive.as_mut()).is_pending());
            ws.abort();
            let Err(error) = receive.await else {
                panic!("the receive succeeded");
            };
            assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
            let mut buf = [0; 1];
            assert_eq!(peer.read(&mut buf).await.unwrap(), 0);
            assert!(ws.send(true, b"late").await.is_err());
        });
    }

    #[test]
    fn a_send_cancelled_mid_frame_ends_sending() {
        futures::executor::block_on(async {
            let (_peer, ws) = server(8);
            {
                let mut send = Box::pin(ws.send(false, &[0; 64]));
                assert!(futures::poll!(send.as_mut()).is_pending());
            }
            let error = ws.send(true, b"next").await.unwrap_err();
            assert!(error.description().contains("mid-message"), "{error:?}");
        });
    }

    #[test]
    fn disconnect_while_a_pong_holds_the_writer_still_ends_the_stream() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server(16);
            // Fill the pipe toward the peer, so answering a ping waits on the peer.
            ws.send(false, &[b'x'; 14]).await.unwrap();
            peer.write_all(&[0x89, 14]).await.unwrap();
            peer.write_all(&[b'p'; 14]).await.unwrap();
            let mut receive = Box::pin(ws.receive(1 << 20));
            assert!(futures::poll!(receive.as_mut()).is_pending());
            ws.disconnect().await;
            let mut wire = Vec::new();
            let read = std::pin::pin!(peer.read_to_end(&mut wire));
            let futures::future::Either::Right((read_result, _)) =
                futures::future::select(receive, read).await
            else {
                panic!("the receive ended");
            };
            read_result.unwrap();
            assert_eq!(wire.len(), 16 + 16);
            assert_eq!(&wire[16..18], &[0x8a, 14]);
        });
    }

    #[test]
    fn abort_interrupts_a_receive_sending_its_protocol_error_close() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server(16);
            ws.send(false, &[b'x'; 14]).await.unwrap();
            // A frame with RSV2 set: the Close owed for it waits on the peer.
            peer.write_all(&[0xa1, 0x00]).await.unwrap();
            let mut receive = Box::pin(ws.receive(1 << 20));
            assert!(futures::poll!(receive.as_mut()).is_pending());
            ws.abort();
            assert!(receive.await.is_err());
        });
    }

    #[test]
    fn a_continuation_is_checked_against_what_is_left_of_the_limit() {
        futures::executor::block_on(async {
            let (mut peer, ws) = server(1 << 16);
            peer.write_all(&[0x02, 0x08]).await.unwrap();
            peer.write_all(&[b'z'; 8]).await.unwrap();
            // The continuation's header alone, declaring 8 more bytes against a limit of 10.
            peer.write_all(&[0x80, 0x08]).await.unwrap();
            let message = ws.receive(10).await.unwrap();
            assert_eq!(message.kind, WsMessageKind::PROTOCOL_ERROR);
            assert_eq!(message.close_code, 1009);
        });
    }
}
