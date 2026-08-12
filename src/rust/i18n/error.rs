// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use thiserror::Error;

/// Errors from the Rust `i18n::transcode` implementation ([`crate::dispatch`]).
///
/// Every message matches the corresponding `JSG_REQUIRE` / `JSG_FAIL_REQUIRE`
/// string in `workerd::api::node::i18n::transcode`
/// (`src/workerd/api/node/i18n.c++`) verbatim, so gate-on and gate-off are
/// indistinguishable to JavaScript. `"Invalid encoding passed to transcode"`
/// is not a variant here: it is raised by the C++ `fromImpl` conversion before
/// the Rust entry point is ever called (see `i18n.c++`), since the bridge
/// `Encoding` enum can only represent the four transcodable encodings.
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
}

impl From<TranscodeError> for jsg::Error {
    fn from(value: TranscodeError) -> Self {
        // All of these are plain JS `Error`s, matching the `JSG_REQUIRE(..., Error, ...)`
        // calls they replace.
        Self::new_error(value.to_string())
    }
}
