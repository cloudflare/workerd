// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The error type reported by [`crate::dispatch`], and its conversion into the
//! JavaScript `Error` the caller of `node:buffer`'s `transcode()` sees.

use thiserror::Error;

/// A failed transcode.
///
/// The messages and JavaScript error types are what `node:buffer`'s
/// `transcode()` exposes to JavaScript, so do not reword them.
/// [`TranscodeError::InvalidEncoding`] shares its message with the
/// `JSG_FAIL_REQUIRE` in `fromImpl` (`src/workerd/api/node/i18n.c++`), and
/// [`TranscodeError::AllocationFailed`] with the one in
/// `jsg::JsUint8Array::create`.
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
    #[error("Failed to initialize converter")]
    ConverterOpenFailed,
    #[error("Setting ICU substitute characters failed")]
    SetSubstituteCharsFailed,
    #[error("Invalid encoding passed to transcode")]
    InvalidEncoding,
    #[error("Failed to allocate memory for Uint8Array")]
    AllocationFailed,
    // Reports a broken internal invariant rather than bad input.
    #[error("Destination buffer size does not match the prepared transcode")]
    DestinationSizeMismatch,
}

impl From<TranscodeError> for jsg::Error {
    fn from(value: TranscodeError) -> Self {
        match value {
            // A `RangeError`, like any other failed `Uint8Array` allocation
            // in `jsg::JsUint8Array::create`.
            TranscodeError::AllocationFailed => Self::new_range_error(value.to_string()),
            _ => Self::new_error(value.to_string()),
        }
    }
}

#[cfg(test)]
#[path = "error-test.rs"]
mod tests;
