#![allow(clippy::unused_async)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::missing_panics_doc)]
#![allow(clippy::missing_safety_doc)]
#![allow(clippy::unnecessary_box_returns)] // cxx bridge functions return Box<T> by contract

mod serve_helpers;
mod test_helpers;

use serve_helpers::ServeEchoSession;
use serve_helpers::start_serve_drop_consumer;
use serve_helpers::start_serve_echo;
use serve_helpers::start_serve_echo_foreign_thread;
use serve_helpers::start_take_socket_echo;
use test_helpers::create_prebound_listener_fd;
use test_helpers::native_write_via_kj_unwrap;
use test_helpers::native_write_via_unwrap;

#[cxx::bridge(namespace = "kj_rs_io_test")]
mod ffi {
    /// A pre-bound, *blocking* std TCP listener handed to C++ as a raw fd (the `--socket-fd`
    /// scenario for `wrapListenSocketFd`).
    struct PreboundListener {
        fd: i32,
        port: u16,
    }

    extern "Rust" {
        /// Recovers the native tokio TcpStream from an unwrapped kj-rs-io stream Box and writes
        /// `data` natively (tokio readiness API, no FFI-per-byte), then closes the connection.
        async fn native_write_via_unwrap(stream: Box<TokioStream>, data: Vec<u8>) -> Result<()>;

        /// Same, but starts from a `kj::AsyncIoStream&`: unwraps it from the Rust side via
        /// `kj_rs_io::unwrap_kj_stream` (the API a native Rust server's glue will use), then writes
        /// natively. Leaves the C++ wrapper hollow.
        async unsafe fn native_write_via_kj_unwrap<'a>(
            stream: Pin<&'a mut KjAsyncIoStream>,
            data: &'a [u8],
        ) -> Result<()>;

        /// Binds 127.0.0.1:0 with std (blocking mode, like an inherited `--socket-fd` listener)
        /// and releases it as a raw fd owned by the caller.
        fn create_prebound_listener_fd() -> Result<PreboundListener>;

        // --- serve_kj_stream (serve_helpers.rs)

        /// One echo server over a served kj stream: `start_serve_echo` picks the transport
        /// path (unwrap fast path or duplex pump) via `kj_rs_io::serve_kj_stream` — taking
        /// ownership of the stream — and spawns the echo consumer on the loop runtime;
        /// `drive()` runs the connection (the pump, on the pumped path) to completion —
        /// dropping the `drive()` promise mid-connection is the abort-on-drop path, which
        /// also destroys the owned stream.
        type ServeEchoSession;

        fn start_serve_echo(stream: KjOwn<KjAsyncIoStream>) -> Box<ServeEchoSession>;

        /// Like `start_serve_echo`, but the consumer reads one message and then DROPS its
        /// `ServeIo` without calling `shutdown()`: the pump must turn the dropped duplex end into
        /// `shutdownWrite()` on the kj stream.
        fn start_serve_drop_consumer(stream: KjOwn<KjAsyncIoStream>) -> Box<ServeEchoSession>;

        /// Like `start_serve_echo`, but the echo consumer runs on a separate OS thread with its
        /// own tokio runtime, driving a pumped stream's `ServeIo::Duplex` end off the KJ
        /// event-loop thread (cross-thread waker path; TSAN target).
        fn start_serve_echo_foreign_thread(stream: KjOwn<KjAsyncIoStream>)
        -> Box<ServeEchoSession>;

        /// Like `start_serve_echo`, but through the native-only `take_kj_socket` entry point
        /// (unwrap tier, else fd-dup tier): errors — instead of pumping — for streams that
        /// are neither. The consumed stream is destroyed before this returns.
        fn start_take_socket_echo(stream: KjOwn<KjAsyncIoStream>) -> Result<Box<ServeEchoSession>>;

        /// Whether the unwrap fast path was taken (perf observability surface).
        fn is_native(self: &ServeEchoSession) -> bool;

        /// Runs the connection to completion (see the type's docs). May only be called once.
        async unsafe fn drive<'a>(self: &'a ServeEchoSession) -> Result<()>;

        /// Resolves once the echo task has exited — observing that dropping `drive()` EOFs
        /// the pumped consumer.
        async unsafe fn wait_echo_done<'a>(self: &'a ServeEchoSession);
    }

    extern "Rust" {
        #[namespace = "kj_rs_io"]
        type TokioStream = kj_rs_io::TokioStream;
    }

    unsafe extern "C++" {
        include!("kj-rs-io/unwrap.h");

        #[namespace = "kj"]
        #[cxx_name = "AsyncIoStream"]
        type KjAsyncIoStream = kj_rs_io::KjAsyncIoStream;
    }
}
