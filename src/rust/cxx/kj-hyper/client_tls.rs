//! Raw-socket TLS for the rust I/O backend: a *synchronous* rustls connection state machine
//! exposed to C++ through sync bridge fns, in both directions:
//!
//! - Client-side (STARTTLS / TLS-from-the-start connects): the raw-TCP-with-TLS path that
//!   `kj::TlsContext::wrapClient()` provides under the default (OpenSSL) build.
//! - Server-side (TLS listeners): the path `kj::TlsContext::wrapPort()`/`wrapServer()` provide
//!   under the default build; the handshake is driven by the first reads (the peer speaks
//!   first).
//!
//! workerd's `RustlsStream` (tls-network.c++) wraps the already-connected plaintext
//! `kj::AsyncIoStream` and drives this state machine over it — all byte I/O and orchestration
//! stays on the C++ side (see `RustlsStream` for the alternation contract), so Rust is a pure,
//! pointer-free, non-async rustls record machine, identical for native and foreign plaintext
//! streams. All outgoing-ciphertext production (`take_tls_out`) is serialized with its wire
//! write by a mutex on the C++ side, so records reach the wire in the sequence-number order
//! rustls produced them (TLS record order is load-bearing for AEAD).

use std::io::Read;

use cxx::KjError;
use cxx::KjExceptionType;
use kj::Result;
use rustls::pki_types::ServerName;

use crate::tls::HyperTlsClientConfig;
use crate::tls::HyperTlsServerConfig;
use crate::tls::kj_error_for_rustls_error;

/// A synchronous rustls TLS connection (client- or server-side). Holds no I/O handles and no
/// pointers: the C++ `RustlsStream` performs all reads/writes on the underlying plaintext
/// stream and feeds this state machine.
pub struct RustlsConn {
    conn: rustls::Connection,
    /// Ciphertext accepted by `feed_tls_in()` but not yet consumed by rustls because its
    /// received-plaintext buffer (a fixed 16 KiB cap; `read_tls` signals this backpressure with
    /// an `ErrorKind::Other` "received plaintext buffer full" error) was full. Drained by
    /// `pump_tls_in()` as `read_plaintext()` frees space, so a wire read larger than one TLS
    /// record (e.g. a bulk download) never turns backpressure into a connection error.
    pending_tls_in: Vec<u8>,
}

/// Build a client TLS connection for `expected_server_hostname` (verified against the server
/// certificate and also sent as SNI — kj's `expectedServerHostname`). Errors if the name is not
/// a valid DNS name / IP address (kj surfaces the equivalent from
/// `X509_VERIFY_PARAM_set1_host()`).
pub(crate) fn new_rustls_client_conn(
    config: &HyperTlsClientConfig,
    expected_server_hostname: &str,
) -> Result<Box<RustlsConn>> {
    let server_name = ServerName::try_from(expected_server_hostname.to_owned()).map_err(|_| {
        KjError::new(
            KjExceptionType::Failed,
            format!("invalid TLS server name \"{expected_server_hostname}\""),
        )
    })?;
    let mut conn = rustls::ClientConnection::new(config.client_config(), server_name)
        .map_err(|e| kj_error_for_rustls_error(&e))?;
    // Accept application plaintext unconditionally: write_plaintext() is always immediately
    // followed by a wire flush (RustlsStream::write -> flushOut), which provides the real
    // backpressure at the socket. Without this, rustls' default send-buffer cap makes a large
    // write_all() (e.g. a paused-reader test writing ahead) fail with "failed to write whole
    // buffer".
    conn.set_buffer_limit(None);
    Ok(Box::new(RustlsConn {
        conn: rustls::Connection::Client(conn),
        pending_tls_in: Vec::new(),
    }))
}

/// Build a server-side TLS connection from the shared rustls server config (the same one the
/// hyper https listener uses). The handshake is driven lazily by the first reads: the peer
/// sends the ClientHello, so a fresh server connection has nothing queued to write.
pub(crate) fn new_rustls_server_conn(config: &HyperTlsServerConfig) -> Result<Box<RustlsConn>> {
    let mut conn = rustls::ServerConnection::new(config.server_config())
        .map_err(|e| kj_error_for_rustls_error(&e))?;
    // See new_rustls_client_conn for why the send-buffer cap is lifted.
    conn.set_buffer_limit(None);
    Ok(Box::new(RustlsConn {
        conn: rustls::Connection::Server(conn),
        pending_tls_in: Vec::new(),
    }))
}

impl RustlsConn {
    /// Whether the TLS handshake is still in progress (kj drives reads/writes until this
    /// clears before delivering/accepting application data).
    pub(crate) fn is_handshaking(&self) -> bool {
        self.conn.is_handshaking()
    }

    /// Whether rustls has ciphertext queued to send on the wire.
    pub(crate) fn wants_tls_write(&self) -> bool {
        self.conn.wants_write()
    }

    /// Serialize all queued outgoing ciphertext (handshake flights, application records,
    /// alerts) into a single buffer to write on the wire. Assigning sequence numbers happens
    /// here, so the C++ side must send the result before calling this again (it serializes
    /// this call with the wire write under a mutex).
    pub(crate) fn take_tls_out(&mut self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        while self.conn.wants_write() {
            self.conn.write_tls(&mut out).map_err(|e| {
                KjError::new(KjExceptionType::Failed, format!("TLS write_tls: {e}"))
            })?;
        }
        Ok(out)
    }

    /// Feed ciphertext read from the wire into rustls and process any completed records.
    /// rustls caps its received-plaintext buffer at 16 KiB and `read_tls` refuses further input
    /// while it is full (backpressure, not failure); anything not consumed here is kept in
    /// `pending_tls_in` and pumped as `read_plaintext()` drains. Errors (bad certificate,
    /// protocol violation, ...) are rendered with kj-parity text.
    pub(crate) fn feed_tls_in(&mut self, data: &[u8]) -> Result<()> {
        self.pending_tls_in.extend_from_slice(data);
        self.pump_tls_in()
    }

    /// Push as much of `pending_tls_in` into rustls as its received-plaintext buffer allows,
    /// processing completed records. Stops (without error) when rustls signals the
    /// buffer-full backpressure condition; the remainder stays pending.
    fn pump_tls_in(&mut self) -> Result<()> {
        while !self.pending_tls_in.is_empty() {
            let mut cursor: &[u8] = &self.pending_tls_in;
            let n = match self.conn.read_tls(&mut cursor) {
                Ok(n) => n,
                // "received plaintext buffer full": documented backpressure signal -- the
                // caller must drain reader() first. Keep the remainder for the next pump.
                Err(e) if e.kind() == std::io::ErrorKind::Other => break,
                Err(e) => {
                    return Err(KjError::new(
                        KjExceptionType::Disconnected,
                        format!("TLS read_tls: {e}"),
                    ));
                }
            };
            if n == 0 {
                break;
            }
            self.pending_tls_in.drain(..n);
            self.conn
                .process_new_packets()
                .map_err(|e| kj_error_for_rustls_error(&e))?;
        }
        Ok(())
    }

    /// Drain decrypted application data into `buf`. Returns the number of bytes read; `0` means
    /// a clean end-of-stream (peer `close_notify`), and `-1` means no plaintext is available
    /// yet (the C++ side must read more ciphertext from the wire and `feed_tls_in()` it).
    pub(crate) fn read_plaintext(&mut self, buf: &mut [u8]) -> Result<i64> {
        loop {
            match self.conn.reader().read(buf) {
                Ok(0) => return Ok(0),
                Ok(n) => {
                    // Space was freed in the received-plaintext buffer: pump any wire bytes
                    // still pending from an earlier feed so progress continues.
                    self.pump_tls_in()?;
                    return Ok(i64::try_from(n).unwrap_or(i64::MAX));
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    // No plaintext buffered. If ciphertext is still pending, push it in and
                    // retry (guaranteed progress: the plaintext buffer is empty, so read_tls
                    // accepts input); otherwise ask the C++ side for more wire data.
                    let before = self.pending_tls_in.len();
                    if before == 0 {
                        return Ok(-1);
                    }
                    self.pump_tls_in()?;
                    if self.pending_tls_in.len() == before {
                        // No progress (e.g. only a partial TLS record is pending): need more
                        // ciphertext from the wire.
                        return Ok(-1);
                    }
                }
                // A peer that closed without close_notify (truncation): treat as EOF, as most
                // TLS peers and applications tolerate it (kj/OpenSSL likewise surface EOF here).
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(0),
                Err(e) => {
                    return Err(KjError::new(
                        KjExceptionType::Disconnected,
                        format!("TLS read: {e}"),
                    ));
                }
            }
        }
    }

    /// Queue application data for encryption. The ciphertext is produced by the next
    /// `take_tls_out()` (rustls buffers it internally until the handshake completes).
    pub(crate) fn write_plaintext(&mut self, data: &[u8]) -> Result<()> {
        use std::io::Write;
        self.conn
            .writer()
            .write_all(data)
            .map_err(|e| KjError::new(KjExceptionType::Failed, format!("TLS write: {e}")))
    }

    /// Queue a `close_notify` alert (kj `shutdownWrite()`); the C++ side flushes it, then
    /// shuts down the underlying write half.
    pub(crate) fn send_close_notify(&mut self) {
        self.conn.send_close_notify();
    }
}
