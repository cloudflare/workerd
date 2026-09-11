//! FFI-island file: the `#[cxx::bridge] mod ffi` for `kj::AsyncInputStream` /
//! `AsyncOutputStream` / `AsyncIoStream`. File-top `#![allow(unsafe_code)]` re-allows unsafe
//! against the crate-root `#![deny(unsafe_code)]`; this file contains ONLY the bridge (every
//! stream shim is already a safe `fn`, so no getter wrappers are needed here). The wholly-safe
//! wrapper TYPES (`AsyncInputStream` read helpers, `AsyncOutputStream`) live in `streams.rs` and
//! are re-exported below. The bridge lives here (rather than a wholly-safe split) because the
//! `kj::io::ffi` module path is referenced across the workerd consumers.
#![allow(unsafe_code)]

// Re-export the wholly-safe wrappers so `kj::io::{AsyncInputStream, AsyncIoStream,
// AsyncOutputStream}` resolve exactly as before the island/wholly-safe split.
pub use crate::streams::*;

#[cxx::bridge(namespace = "kj::rust")]
pub mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");

        type AsyncInputStream;
        type AsyncIoStream;
        type AsyncOutputStream;

        /// Corresponds to `kj::AsyncInputStream::tryRead(buffer, minBytes, buffer.size())`.
        /// Resolves to the number of bytes read; fewer than `min_bytes` (possibly zero)
        /// indicates EOF.
        async fn async_input_stream_try_read(
            this_: Pin<&mut AsyncInputStream>,
            buffer: &mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncInputStream::tryGetLength()`.
        fn async_input_stream_try_get_length(this_: Pin<&mut AsyncInputStream>) -> KjMaybe<u64>;

        async fn async_output_stream_write(
            this_: Pin<&mut AsyncOutputStream>,
            buffer: &[u8],
        ) -> Result<()>;

        async fn async_output_stream_when_write_disconnected(
            this_: Pin<&mut AsyncOutputStream>,
        ) -> Result<()>;

        /// Corresponds to `kj::AsyncIoStream::tryRead(buffer, minBytes, buffer.size())`.
        /// Two-way-stream variant of `async_input_stream_try_read`: takes the stream itself
        /// so callers can read and write the same stream concurrently (through separate
        /// raw-pointer-derived views; two `Pin<&mut>` views of the split bases cannot
        /// coexist through the bridge).
        async fn async_io_stream_try_read(
            this_: Pin<&mut AsyncIoStream>,
            buffer: &mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;

        /// Corresponds to `kj::AsyncIoStream::write(buffer)`.
        async fn async_io_stream_write(this_: Pin<&mut AsyncIoStream>, buffer: &[u8])
        -> Result<()>;

        /// Corresponds to `kj::AsyncIoStream::shutdownWrite()`.
        fn async_io_stream_shutdown_write(this_: Pin<&mut AsyncIoStream>);
    }
}
