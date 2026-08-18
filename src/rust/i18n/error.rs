// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The error type reported by [`crate::dispatch`], and its adapter for the
//! trip across the CXX bridge.

use thiserror::Error;

/// A failed transcode.
///
/// Every message is the text of the JavaScript `Error` that reaches the
/// caller of `node:buffer`'s `transcode()`.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum TranscodeError {
    #[error("Source buffer is too large to transcode")]
    SourceBufferTooLarge,
    #[error("Buffer is too large to transcode")]
    BufferTooLarge,
    #[error("Expected UTF-16le length is too large to transcode")]
    ExpectedUtf16LengthTooLarge,
    #[error("Expected UTF-8 length is too large to transcode")]
    ExpectedUtf8LengthTooLarge,
    #[error("UTF-16le input size should be multiple of 2")]
    OddUtf16leInput,
    #[error("Expected UTF16 length mismatch")]
    Utf16LengthMismatch,
    #[error("Expected UTF8 length mismatch")]
    Utf8LengthMismatch,
    #[error("Unable to transcode buffer")]
    UnableToTranscode,
    #[error("Invalid encoding passed to transcode")]
    InvalidEncoding,
    #[error("Failed to initialize converter")]
    ConverterOpenFailed,
    #[error("Setting ICU substitute characters failed")]
    SetSubstituteCharsFailed,
    #[error("Destination buffer size does not match the prepared transcode")]
    DestinationSizeMismatch,
}

/// A [`TranscodeError`] on its way out through the CXX bridge.
///
/// `cxx` converts a returned `Err` into a `kj::Exception` whose description is
/// the error's `Display` output. A description that begins with
/// `jsg.<ErrorType>: ` tells JSG to throw that JavaScript error type using the
/// remaining text as the message (see `tunneledErrorType` in
/// `src/workerd/jsg/exception.c++`); this is the same encoding
/// `JSG_REQUIRE(..., Error, ...)` produces. Emitting the prefix here is
/// therefore what turns a [`TranscodeError`] into a JavaScript `Error` whose
/// `message` is the variant's text.
#[derive(Debug, Error)]
#[error("jsg.Error: {0}")]
pub struct JsError(#[from] TranscodeError);
