// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Bodies between KJ streams and [`Body`], on the loop thread.
//!
//! Each pump is a future the adapter's request owns: it borrows the KJ stream for as long as it
//! runs and is dropped with the request, so a cancelled request touches no KJ stream again. Bytes
//! are copied once per direction; nothing here is KJ's `tryPumpFrom`, which hands a whole stream
//! to another without the bytes passing through the caller.

use std::pin::Pin;
use std::pin::pin;

use bytes::Bytes;
use futures::future::Either;
use futures::future::select;
use kj::io::AsyncInputStream;
use kj::io::AsyncOutputStream;

use crate::Body;
use crate::BodySender;
use crate::Result;

/// The most a single read takes from a KJ stream, and so the largest chunk a pump makes.
pub const READ_SIZE: usize = 64 * 1024;

/// A KJ stream to read a body from.
pub trait Source {
    /// The stream's `tryRead` with a minimum of one byte: the count read into `buffer`, zero at
    /// the stream's end.
    async fn read(&mut self, buffer: &mut [u8]) -> Result<usize>;
}

impl<S: Source> Source for &mut S {
    async fn read(&mut self, buffer: &mut [u8]) -> Result<usize> {
        S::read(self, buffer).await
    }
}

impl Source for Pin<&mut AsyncInputStream> {
    async fn read(&mut self, buffer: &mut [u8]) -> Result<usize> {
        self.as_mut().try_read(buffer, 1).await
    }
}

/// Reads a KJ stream into `sender` until the stream ends, which finishes the body, or fails,
/// which fails the body. Returns early, leaving the stream unread, once the body's reader is
/// gone.
pub async fn kj_to_send(mut source: impl Source, mut sender: BodySender) {
    let mut buffer = vec![0; READ_SIZE];
    loop {
        let count = {
            let reading = pin!(source.read(&mut buffer));
            let closed = pin!(sender.closed());
            match select(reading, closed).await {
                Either::Left((count, _)) => count,
                Either::Right(((), _)) => return,
            }
        };
        match count {
            Ok(0) => return sender.finish(),
            Ok(count) => {
                // A chunk of its own, so that the channel bounds what the body holds.
                let chunk = Bytes::copy_from_slice(&buffer[..count]);
                if sender.send(chunk).await.is_err() {
                    return;
                }
            }
            Err(error) => return sender.fail(error).await,
        }
    }
}

/// Writes `body` to a KJ stream, one write per chunk, each awaited before the next chunk is
/// taken.
///
/// # Errors
///
/// The body's own error, or the stream's.
pub async fn send_to_kj(mut body: Body, output: &mut AsyncOutputStream<'_>) -> Result<()> {
    while let Some(chunk) = body.chunk().await {
        output.write(&chunk?).await?;
    }
    Ok(())
}
