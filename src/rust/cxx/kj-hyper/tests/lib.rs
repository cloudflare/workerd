//! Rust side of kj-hyper's C++-driven tests: the bridge the `KJ_TEST`s call into, and the
//! helpers behind it (`serve_helpers.rs`).
// cxx bridge functions return Box<T> by contract.
#![expect(clippy::unnecessary_box_returns)]

mod serve_helpers;

use serve_helpers::NativeServeFailure;
use serve_helpers::ServeEchoSession;
use serve_helpers::expect_serve_stream_failure;
use serve_helpers::expect_take_socket_failure;
use serve_helpers::start_serve_drop_consumer;
use serve_helpers::start_serve_echo;
use serve_helpers::start_serve_echo_foreign_thread;
use serve_helpers::start_serve_write_then_drop;
use serve_helpers::start_take_socket_echo;
use serve_helpers::take_socket_failure_dropping_stream;

#[cxx::bridge(namespace = "kj_hyper_test")]
mod ffi {

    extern "Rust" {
        // --- kj_hyper::serve (serve_helpers.rs)

        /// One echo server over a served kj stream: `start_serve_echo` picks the transport
        /// path (native unwrap, or the kj stream driven directly) via
        /// `kj_hyper::serve::serve_kj_stream` -- taking ownership of the stream -- and spawns the
        /// echo consumer on the loop runtime; `drive()` runs the consumer to completion --
        /// dropping the `drive()` promise mid-connection aborts it, which also destroys the
        /// owned stream.
        type ServeEchoSession;

        fn start_serve_echo(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>>;

        /// Like `start_serve_echo`, but the consumer reads one message and then DROPS its
        /// `ServeIo` without calling `shutdown()`: dropping it destroys the kj stream, which the
        /// peer sees as EOF.
        fn start_serve_drop_consumer(stream: KjOwn<AsyncIoStream>)
        -> Result<Box<ServeEchoSession>>;

        /// Like `start_serve_drop_consumer`, but the consumer WRITES a large payload and then
        /// drops its `ServeIo` without reading: with a kj peer that never reads, the kj write
        /// blocks, and the consumer's drop must cancel it.
        fn start_serve_write_then_drop(
            stream: KjOwn<AsyncIoStream>,
        ) -> Result<Box<ServeEchoSession>>;

        /// Like `start_serve_echo`, but the echo consumer runs on a separate OS thread with its
        /// own tokio runtime: a kj stream polled off its event loop's thread, which must fail
        /// without touching the stream.
        fn start_serve_echo_foreign_thread(
            stream: KjOwn<AsyncIoStream>,
        ) -> Result<Box<ServeEchoSession>>;

        /// Like `start_serve_echo`, but through the native-only `take_kj_socket` entry point
        /// (unwrap path only): errors -- instead of driving them -- for foreign streams. The consumed
        /// stream is destroyed before this returns.
        fn start_take_socket_echo(stream: KjOwn<AsyncIoStream>) -> Result<Box<ServeEchoSession>>;

        /// Requires native socket extraction to reject an in-flight operation while retaining
        /// ownership of the untouched stream.
        type NativeServeFailure;

        fn expect_take_socket_failure(stream: KjOwn<AsyncIoStream>) -> Box<NativeServeFailure>;

        fn expect_serve_stream_failure(stream: KjOwn<AsyncIoStream>) -> Box<NativeServeFailure>;

        fn is_in_flight(self: &NativeServeFailure) -> bool;

        /// Attempts `take_kj_socket` and, on failure, converts the error to a plain `KjError`
        /// -- which DROPS the handed-back stream -- returning the description. With a read in
        /// flight this is the realistic "caller just propagates the error" path; it must be
        /// memory-safe because the pending read owns its share of the socket.
        fn take_socket_failure_dropping_stream(stream: KjOwn<AsyncIoStream>) -> String;

        fn take_stream(self: &NativeServeFailure) -> KjOwn<AsyncIoStream>;

        /// Whether the unwrap fast path was taken (perf observability surface).
        fn is_native(self: &ServeEchoSession) -> bool;

        /// Runs the connection to completion (see the type's docs). May only be called once.
        async unsafe fn drive<'a>(self: &'a ServeEchoSession) -> Result<()>;

        /// Resolves once the echo task has exited, however it ended.
        async unsafe fn wait_echo_done<'a>(self: &'a ServeEchoSession);
    }

    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
    }
}
