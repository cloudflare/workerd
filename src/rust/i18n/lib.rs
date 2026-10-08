// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust port of `workerd::api::node::i18n::transcode`
//! (`src/workerd/api/node/i18n.c++`), the engine behind `node:buffer`'s
//! `transcode()`. Selected at runtime by the `NODEJS_I18N_RUST` autogate; when
//! the gate is off, the C++ implementation is used instead. The two paths are
//! byte-for-byte and error-message identical: [`dispatch`] ports the C++
//! dispatch/sizing/truncation logic to Rust, while [`codecs`] reimplements, in
//! safe Rust, the ICU conversions and simdutf primitives the C++ path calls.
//! `src/workerd/api/node/i18n-test.c++` checks the two paths against each
//! other.

use jsg::Lock;
use jsg::v8;

mod codecs;
mod dispatch;
mod error;
mod ffi;

use crate::dispatch::Transcoder;
use crate::error::TranscodeError;

/// Transcodes `source` into a freshly allocated `Uint8Array`.
///
/// The conversion writes straight into the V8 `ArrayBuffer`'s backing store.
/// There is no intermediate `Vec` and no copy: [`Transcoder`] reports the
/// destination size up front, the buffer is allocated at exactly that size,
/// and the returned view is narrowed to the bytes actually written.
fn transcode_impl<'a>(
    lock: &'a mut Lock,
    source: &[u8],
    from_encoding: ffi::Encoding,
    to_encoding: ffi::Encoding,
) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
    let transcoder = Transcoder::new(source, from_encoding, to_encoding)?;
    let (buffer, written) = v8::ArrayBuffer::new_zeroed_with(lock, transcoder.dest_len(), |dest| {
        transcoder.transcode_into(dest)
    })
    .ok_or(TranscodeError::AllocationFailed)?;
    v8::Uint8Array::from_buffer(lock, &buffer, 0, written?)
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
