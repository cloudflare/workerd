//! Rust helpers for the `kj_hyper::serve` `KJ_TEST`s (`serve-test.c++`): echo sessions over
//! each transport path, driven by the C++ tests.

use std::cell::RefCell;

use cxx::KjError;
use kj_hyper::serve::ServeIo;
use kj_hyper::serve::TakeSocketError;
use kj_hyper::serve::serve_kj_stream;
use kj_hyper::serve::take_kj_socket;
use kj_rs::KjOwn;
use tokio::io::AsyncWriteExt;
use tokio::sync::watch;

use crate::ffi::AsyncIoStream;

type Result<T> = std::result::Result<T, KjError>;

fn is_disconnect(error: &std::io::Error) -> bool {
    matches!(
        error.kind(),
        std::io::ErrorKind::BrokenPipe
            | std::io::ErrorKind::ConnectionReset
            | std::io::ErrorKind::ConnectionAborted
    )
}

fn kj_err(message: impl std::fmt::Display) -> KjError {
    KjError::new(cxx::KjExceptionType::Failed, message.to_string())
}

/// A native-serving failure whose stream remains owned until the C++ test has cancelled the
/// operation that prevented extraction.
pub struct NativeServeFailure {
    stream: RefCell<Option<KjOwn<AsyncIoStream>>>,
    in_flight: bool,
}

impl NativeServeFailure {
    fn from_error(error: TakeSocketError) -> Box<Self> {
        let in_flight = error.error.description().contains("in flight");
        Box::new(Self {
            stream: RefCell::new(Some(error.stream)),
            in_flight,
        })
    }
}

/// See the bridge doc in lib.rs: the error is converted with `KjError::from`, which drops the
/// handed-back stream while the caller's read is still pending.
pub fn take_socket_failure_dropping_stream(stream: KjOwn<AsyncIoStream>) -> String {
    match take_kj_socket(stream) {
        Ok(_) => panic!("taking a native socket with I/O in flight unexpectedly succeeded"),
        Err(error) => KjError::from(error).description().to_owned(),
    }
}

pub fn expect_take_socket_failure(stream: KjOwn<AsyncIoStream>) -> Box<NativeServeFailure> {
    match take_kj_socket(stream) {
        Ok(_) => panic!("taking a native socket with I/O in flight unexpectedly succeeded"),
        Err(error) => NativeServeFailure::from_error(error),
    }
}

pub fn expect_serve_stream_failure(stream: KjOwn<AsyncIoStream>) -> Box<NativeServeFailure> {
    match serve_kj_stream(stream) {
        Ok(_) => panic!("serving a native stream with I/O in flight unexpectedly succeeded"),
        Err(error) => NativeServeFailure::from_error(error),
    }
}

impl NativeServeFailure {
    pub fn is_in_flight(&self) -> bool {
        self.in_flight
    }

    pub fn take_stream(&self) -> KjOwn<AsyncIoStream> {
        self.stream
            .borrow_mut()
            .take()
            .expect("failed native-serving stream already taken")
    }
}

/// Signals the session's completion watch when the consumer ends -- normally, with an error, or
/// by being aborted (dropping `drive()`).
struct DoneGuard(watch::Sender<bool>);

impl Drop for DoneGuard {
    fn drop(&mut self) {
        let _ = self.0.send(true);
    }
}

/// The echo task shared by every path: copy everything read back to the writer, then propagate
/// the half-close.
async fn echo(io: ServeIo, _done: DoneGuard) -> std::io::Result<u64> {
    async {
        let (mut rd, mut wr) = tokio::io::split(io);
        // A peer that went away mid-echo is a normal end for an echo server: a kj stream's
        // DISCONNECTED write surfaces as a broken pipe.
        let n = match tokio::io::copy(&mut rd, &mut wr).await {
            Ok(n) => n,
            Err(e) if is_disconnect(&e) => return Ok(0),
            Err(e) => return Err(e),
        };
        match wr.shutdown().await {
            Ok(()) => {}
            Err(e) if is_disconnect(&e) => {}
            Err(e) => return Err(e),
        }
        Ok(n)
    }
    .await
}

/// A consumer that reads one message and then drops the stream WITHOUT `shutdown()` (see
/// `start_serve_drop_consumer` in lib.rs).
async fn read_then_drop(mut io: ServeIo, _done: DoneGuard) -> std::io::Result<u64> {
    use tokio::io::AsyncReadExt;
    let mut buf = [0u8; 64];
    let n = io.read(&mut buf).await?;
    drop(io);
    Ok(n as u64)
}

/// A consumer that writes a large payload and then drops the stream without ever reading (see
/// `start_serve_write_then_drop` in lib.rs).
async fn write_then_drop(mut io: ServeIo, _done: DoneGuard) -> std::io::Result<u64> {
    use tokio::io::AsyncWriteExt;
    let payload = vec![0x5au8; 1024 * 1024];
    // With a peer that never reads, this write blocks until the kj stream takes it -- which is
    // the situation under test -- so give up on it after a moment and drop the stream mid-write.
    let written = match tokio::time::timeout(
        std::time::Duration::from_millis(200),
        io.write_all(&payload),
    )
    .await
    {
        Ok(Ok(())) => payload.len() as u64,
        Ok(Err(_)) | Err(_) => 0,
    };
    drop(io);
    Ok(written)
}

/// One consumer over a served kj stream, running as a loop-runtime task (or, for the
/// foreign-thread variant, on its own OS thread); see `start_serve_echo` in lib.rs.
pub struct ServeEchoSession {
    native: bool,
    /// The consumer's completion signal (fires however it ends).
    done_rx: watch::Receiver<bool>,
    /// Taken by `drive()`.
    echo: RefCell<Option<tokio::task::JoinHandle<std::io::Result<u64>>>>,
    /// For the foreign-thread variant: the OS thread running the consumer, joined by `drive()`.
    foreign: RefCell<Option<std::thread::JoinHandle<std::io::Result<u64>>>>,
}

/// Whether `serve_kj_stream` took a native socket (as opposed to driving the kj stream).
fn is_native(io: &ServeIo) -> bool {
    !matches!(io, ServeIo::Kj(_))
}

impl ServeEchoSession {
    fn spawn<F>(io: ServeIo, consumer: impl FnOnce(ServeIo, DoneGuard) -> F) -> Box<Self>
    where
        F: std::future::Future<Output = std::io::Result<u64>> + 'static,
    {
        let native = is_native(&io);
        let (done_tx, done_rx) = watch::channel(false);
        // The consumer runs on this thread's KJ-loop runtime, where the kj stream lives.
        let echo = kj_rs_tokio::spawn(consumer(io, DoneGuard(done_tx)));
        Box::new(Self {
            native,
            done_rx,
            echo: RefCell::new(Some(echo)),
            foreign: RefCell::new(None),
        })
    }
}

pub fn start_serve_echo(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>> {
    let io = serve_kj_stream(stream).map_err(KjError::from)?;
    Ok(ServeEchoSession::spawn(io, echo))
}

pub fn start_serve_write_then_drop(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>> {
    let io = serve_kj_stream(stream).map_err(KjError::from)?;
    Ok(ServeEchoSession::spawn(io, write_then_drop))
}

pub fn start_serve_drop_consumer(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>> {
    let io = serve_kj_stream(stream).map_err(KjError::from)?;
    Ok(ServeEchoSession::spawn(io, read_then_drop))
}

/// Like `start_serve_echo`, but the echo consumer runs on another OS thread with its own tokio
/// runtime: a kj stream driven off the thread owning its event loop, which must fail rather than
/// touch the stream.
pub fn start_serve_echo_foreign_thread(
    stream: KjOwn<AsyncIoStream>,
) -> Result<Box<ServeEchoSession>> {
    let io = serve_kj_stream(stream).map_err(KjError::from)?;
    let native = is_native(&io);
    let (done_tx, done_rx) = watch::channel(false);
    let foreign = std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("build echo-consumer runtime");
        rt.block_on(echo(io, DoneGuard(done_tx)))
    });
    Ok(Box::new(ServeEchoSession {
        native,
        done_rx,
        echo: RefCell::new(None),
        foreign: RefCell::new(Some(foreign)),
    }))
}

/// Like `start_serve_echo`, but through the native-only entry point (`take_kj_socket`): errors
/// for streams that are not kj-rs-io sockets (the handed-back stream is dropped with the error).
pub fn start_take_socket_echo(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>> {
    let io = take_kj_socket(stream).map_err(KjError::from)?;
    Ok(ServeEchoSession::spawn(io, echo))
}

/// Aborts the consumer task when `drive()` is dropped mid-connection.
struct AbortOnDrop(tokio::task::AbortHandle);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

impl ServeEchoSession {
    pub fn is_native(&self) -> bool {
        self.native
    }

    /// Runs the consumer to completion. Dropping the returned promise mid-connection aborts the
    /// consumer, which drops its stream -- the abort-on-drop path.
    pub async fn drive(&self) -> Result<()> {
        let echo = self.echo.borrow_mut().take();
        if let Some(handle) = echo {
            let _abort = AbortOnDrop(handle.abort_handle());
            handle
                .await
                .map_err(|e| kj_err(format!("echo task panicked: {e}")))?
                .map_err(|e| kj_err(format!("echo failed: {e}")))?;
        }
        let foreign = self.foreign.borrow_mut().take();
        if let Some(handle) = foreign {
            self.wait_echo_done().await;
            handle
                .join()
                .map_err(|_| kj_err("echo consumer thread panicked"))?
                .map_err(|e| kj_err(format!("echo failed: {e}")))?;
        }
        Ok(())
    }

    /// Resolves once the consumer has finished, however it ended.
    pub async fn wait_echo_done(&self) {
        let mut rx = self.done_rx.clone();
        let _ = rx.wait_for(|done| *done).await;
    }
}
