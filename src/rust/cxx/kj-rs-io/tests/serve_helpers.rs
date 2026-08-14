//! Rust helpers for the `serve_kj_stream` `KJ_TEST`s (`serve-test.c++`):
//! echo sessions over each transport path, driven by the C++ tests.

use std::cell::RefCell;

use cxx::KjError;
use kj_rs::KjOwn;
use kj_rs_io::serve::ServeIo;
use kj_rs_io::serve::ServePath;
use kj_rs_io::serve::StreamPump;
use tokio::io::AsyncWriteExt;
use tokio::sync::watch;

use crate::ffi::KjAsyncIoStream;

type Result<T> = std::result::Result<T, KjError>;

fn kj_err(message: impl std::fmt::Display) -> KjError {
    KjError::new(cxx::KjExceptionType::Failed, message.to_string())
}

/// The echo task shared by every path: copy everything read back to the writer, then
/// propagate the half-close. Reports completion through `done_tx`.
async fn echo(io: ServeIo, done_tx: watch::Sender<bool>) -> std::io::Result<u64> {
    let result = async {
        let (mut rd, mut wr) = tokio::io::split(io);
        let n = tokio::io::copy(&mut rd, &mut wr).await?;
        wr.shutdown().await?;
        Ok(n)
    }
    .await;
    let _ = done_tx.send(true);
    result
}

/// A consumer that reads one message and then drops the stream WITHOUT `shutdown()` (see
/// `start_serve_drop_consumer` in lib.rs). Reports completion through `done_tx`.
async fn read_then_drop(mut io: ServeIo, done_tx: watch::Sender<bool>) -> std::io::Result<u64> {
    use tokio::io::AsyncReadExt;
    let result = async {
        let mut buf = [0u8; 64];
        let n = io.read(&mut buf).await?;
        drop(io);
        Ok(n as u64)
    }
    .await;
    let _ = done_tx.send(true);
    result
}

/// One echo server over a served kj stream; see `start_serve_echo` in lib.rs.
pub struct ServeEchoSession {
    native: bool,
    /// Present for the pumped path; taken by `drive()`.
    pump: RefCell<Option<StreamPump>>,
    /// The echo task's completion signal (fires even if the task failed).
    done_rx: watch::Receiver<bool>,
    /// Taken by `drive()`.
    echo: RefCell<Option<tokio::task::JoinHandle<std::io::Result<u64>>>>,
    /// For the foreign-thread variant: the OS thread running the echo consumer, joined by
    /// `drive()`. `None` for the loop-runtime variant.
    foreign: RefCell<Option<std::thread::JoinHandle<std::io::Result<u64>>>>,
}

pub fn start_serve_echo(stream: KjOwn<KjAsyncIoStream>) -> Box<ServeEchoSession> {
    let served = kj_rs_io::serve_kj_stream(stream);
    Box::new(ServeEchoSession::new(served.io, served.pump))
}

pub fn start_serve_drop_consumer(stream: KjOwn<KjAsyncIoStream>) -> Box<ServeEchoSession> {
    let served = kj_rs_io::serve_kj_stream(stream);
    let native = served.io.path() == ServePath::Native;
    let (done_tx, done_rx) = watch::channel(false);
    let consumer = kj_rs_tokio::spawn(read_then_drop(served.io, done_tx));
    Box::new(ServeEchoSession {
        native,
        pump: RefCell::new(served.pump),
        done_rx,
        echo: RefCell::new(Some(consumer)),
        foreign: RefCell::new(None),
    })
}

/// Like `start_serve_echo`, but the echo consumer runs on a SEPARATE OS thread with its own
/// plain tokio runtime instead of this thread's KJ-loop runtime. For a pumped (Duplex) stream
/// this drives the `ServeIo::Duplex` end entirely off the KJ event-loop thread: every read /
/// write / drop on it wakes the pump (parked on the KJ loop) cross-thread through the kj-rs
/// `FutureWakerCell`. That is the scenario `ServedKjStream::io`'s docs describe as legal since
/// the waker bridge became thread-safe; this proves it (and is a TSAN target).
pub fn start_serve_echo_foreign_thread(stream: KjOwn<KjAsyncIoStream>) -> Box<ServeEchoSession> {
    let served = kj_rs_io::serve_kj_stream(stream);
    Box::new(ServeEchoSession::new_foreign_thread(served.io, served.pump))
}

/// Like `start_serve_echo`, but through the native-only entry point (`take_kj_socket`,
/// tiers 1 + 2): errors for streams that are neither kj-rs-io native nor fd-backed (the
/// handed-back stream is dropped with the error).
pub fn start_take_socket_echo(stream: KjOwn<KjAsyncIoStream>) -> Result<Box<ServeEchoSession>> {
    let io = kj_rs_io::take_kj_socket(stream).map_err(KjError::from)?;
    Ok(Box::new(ServeEchoSession::new(io, None)))
}

impl ServeEchoSession {
    fn new(io: ServeIo, pump: Option<StreamPump>) -> Self {
        let native = io.path() == ServePath::Native;
        let (done_tx, done_rx) = watch::channel(false);
        // The echo consumer runs on this thread's KJ-loop runtime: the one-runtime shape
        // (native sockets are registered with this runtime's I/O driver anyway).
        let echo = kj_rs_tokio::spawn(echo(io, done_tx));
        Self {
            native,
            pump: RefCell::new(pump),
            done_rx,
            echo: RefCell::new(Some(echo)),
            foreign: RefCell::new(None),
        }
    }

    fn new_foreign_thread(io: ServeIo, pump: Option<StreamPump>) -> Self {
        let native = io.path() == ServePath::Native;
        let (done_tx, done_rx) = watch::channel(false);
        // Run the echo consumer on its own OS thread with a private current-thread runtime, so
        // the Duplex end is driven entirely off the KJ event-loop thread.
        let foreign = std::thread::spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("build echo-consumer runtime");
            rt.block_on(echo(io, done_tx))
        });
        Self {
            native,
            pump: RefCell::new(pump),
            done_rx,
            echo: RefCell::new(None),
            foreign: RefCell::new(Some(foreign)),
        }
    }

    pub fn is_native(&self) -> bool {
        self.native
    }

    /// Runs the connection to completion: drives the pump (if any) and waits for the echo
    /// task to finish. Dropping the returned promise mid-connection drops the pump —
    /// the abort-on-drop path.
    pub async fn drive(&self) -> Result<()> {
        let pump = self.pump.borrow_mut().take();
        let echo = self.echo.borrow_mut().take();
        // Await the pump but do NOT `?` yet: on the foreign-thread variant we must still join
        // the consumer OS thread even if the pump errored, or we leak an unjoined thread.
        let pump_result = match pump {
            Some(pump) => pump.await,
            None => Ok(()),
        };
        if let Some(handle) = echo {
            handle
                .await
                .map_err(|e| kj_err(format!("echo task panicked: {e}")))?
                .map_err(|e| kj_err(format!("echo failed: {e}")))?;
        }
        // Foreign-thread variant: wait for the consumer to finish (dropping the pump above
        // EOFs it), then join. Waiting on the done signal first keeps the join near-instant.
        let foreign = self.foreign.borrow_mut().take();
        if let Some(handle) = foreign {
            self.wait_echo_done().await;
            handle
                .join()
                .map_err(|_| kj_err("echo consumer thread panicked"))?
                .map_err(|e| kj_err(format!("echo failed: {e}")))?;
        }
        pump_result?;
        Ok(())
    }

    /// Resolves once the echo task has finished (however `drive()` fared) — used by the
    /// drop-abort test to observe that dropping the pump EOFs the consumer.
    pub async fn wait_echo_done(&self) {
        let mut rx = self.done_rx.clone();
        // Cannot fail: the sender is owned by the echo task, which always sends before exit;
        // even if it panicked, the closed channel resolves wait_for with an error we ignore
        // after checking the flag.
        let _ = rx.wait_for(|done| *done).await;
    }
}
