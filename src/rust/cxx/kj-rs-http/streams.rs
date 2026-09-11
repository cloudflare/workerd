//! Wholly-safe wrappers for the KJ async streams: the `AsyncInputStream` read/length helpers and
//! the owned-or-borrowed `AsyncOutputStream`.
//!
//! This file inherits the crate-root `#![deny(unsafe_code)]` — it carries no `#![allow(unsafe_code)]`
//! and contains ZERO `unsafe`. Every FFI call here is a safe bridge shim exposed by the `io` island
//! (`crate::io::ffi::async_*`). Re-exported from `crate::io` so the `kj::io::{AsyncInputStream,
//! AsyncIoStream, AsyncOutputStream}` public paths are unchanged.

use std::pin::Pin;

use kj_rs::KjOwn;

use crate::OwnOrMut;
use crate::Result;
use crate::io::ffi;

pub type AsyncInputStream = ffi::AsyncInputStream;
pub type AsyncIoStream = ffi::AsyncIoStream;

impl AsyncInputStream {
    /// Read up to `buffer.len()` bytes into `buffer`, waiting until at least `min_bytes` are
    /// available. Returns the number of bytes read; a result of fewer than `min_bytes` (possibly
    /// zero) indicates the stream reached EOF.
    ///
    /// The buffer is only written through the returned future; dropping the future cancels the
    /// underlying `kj::Promise` synchronously, after which the buffer is no longer accessed.
    pub async fn try_read(
        self: Pin<&mut Self>,
        buffer: &mut [u8],
        min_bytes: usize,
    ) -> Result<usize> {
        Ok(ffi::async_input_stream_try_read(self, buffer, min_bytes).await?)
    }

    /// Total length of the stream if known in advance (e.g. from `Content-Length`).
    pub fn try_get_length(self: Pin<&mut Self>) -> Option<u64> {
        ffi::async_input_stream_try_get_length(self).into()
    }
}

/// Owned-or-borrowed wrapper for `kj::AsyncOutputStream`.
pub struct AsyncOutputStream<'a>(OwnOrMut<'a, ffi::AsyncOutputStream>);

impl AsyncOutputStream<'_> {
    pub async fn write(&mut self, buffer: &[u8]) -> Result<()> {
        let stream = self.0.as_mut();
        ffi::async_output_stream_write(stream, buffer).await?;
        Ok(())
    }

    pub async fn when_write_disconnected(&mut self) -> Result<()> {
        let stream = self.0.as_mut();
        ffi::async_output_stream_when_write_disconnected(stream).await?;
        Ok(())
    }
}

impl From<KjOwn<ffi::AsyncOutputStream>> for AsyncOutputStream<'_> {
    fn from(value: KjOwn<ffi::AsyncOutputStream>) -> Self {
        Self(value.into())
    }
}

impl<'a> From<Pin<&'a mut ffi::AsyncOutputStream>> for AsyncOutputStream<'a> {
    fn from(value: Pin<&'a mut ffi::AsyncOutputStream>) -> Self {
        Self(value.into())
    }
}
