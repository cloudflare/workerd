// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The error type reported by [`crate::dispatch`], and its conversion into the
//! JavaScript `Error` the caller of `node:buffer`'s `transcode()` sees.

use thiserror::Error;

/// A failed transcode.
///
/// The messages of the variants that have a C++ counterpart match the
/// corresponding `JSG_REQUIRE` / `JSG_FAIL_REQUIRE` string in
/// `workerd::api::node::i18n::transcode` (`src/workerd/api/node/i18n.c++`)
/// verbatim, so gate-on and gate-off are indistinguishable to JavaScript.
/// Do not reword them.
///
/// `"Invalid encoding passed to transcode"` has no variant here: the C++
/// `fromImpl` conversion raises it before the Rust entry point is reached,
/// since the bridge `Encoding` enum can only name the four transcodable
/// encodings.
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
    // The remaining variants report broken internal invariants rather than bad
    // input, and so have no C++ counterpart to match.
    #[error("Invalid encoding passed to transcode")]
    InvalidEncoding,
    #[error("Destination buffer size does not match the prepared transcode")]
    DestinationSizeMismatch,
    #[error("Failed to allocate transcode destination buffer")]
    AllocationFailed,
}

impl From<TranscodeError> for jsg::Error {
    fn from(value: TranscodeError) -> Self {
        // All of these are plain JS `Error`s, matching the
        // `JSG_REQUIRE(..., Error, ...)` calls they replace.
        Self::new_error(value.to_string())
    }
}
