// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Decodes the aws-chunked content encoding of S3 streaming uploads, for C++'s
//! `AwsChunkedDecoderStream` (`src/workerd/api/streams/aws-chunked.h`).
//!
//! C++ drives the [`Decoder`] from a `SyncTransformStage`: it pushes each write, takes the
//! decoded output, and ends it once the input ends. Errors are `TypeError`s and `RangeError`s,
//! described as JSG expects (`jsg.TypeError: ...`) so they reach JavaScript as such.

mod decoder;

pub use decoder::Field;
pub use decoder::Malformed;
pub use decoder::Options;

#[cxx::bridge(namespace = "workerd::api::aws_chunked")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    /// A trailer field.
    #[derive(Debug)]
    struct TrailerField {
        name: String,
        value: String,
    }

    /// The decoder's options, as JavaScript passed them. Each `has_` flag says whether the
    /// value after it was given.
    struct DecoderOptions {
        has_decoded_length: bool,
        decoded_length: f64,
        has_allowed_trailers: bool,
        allowed_trailers: Vec<String>,
        has_max_trailer_bytes: bool,
        max_trailer_bytes: f64,
        drain_on_error: bool,
    }

    extern "Rust" {
        type Decoder;

        /// Validates `options` and creates a decoder.
        fn new_decoder(options: DecoderOptions) -> Result<Box<Decoder>>;

        /// Decodes `input`. The decoded data is taken with `take_output()`, also after a failure,
        /// when it is the data decoded before the error.
        fn push(self: &mut Decoder, input: &[u8]) -> Result<()>;

        /// Finishes the body, returning the trailer fields that are allowed.
        fn end(self: &mut Decoder) -> Result<Vec<TrailerField>>;

        /// Takes the data the last `push()` decoded.
        fn take_output(self: &mut Decoder) -> Vec<u8>;

        /// The decoded length, if it was given.
        fn decoded_length(self: &Decoder) -> KjMaybe<u64>;

        /// The bytes the decoder holds between calls, for memory accounting.
        fn retained_bytes(self: &Decoder) -> usize;
    }
}

/// The default bound on the trailer section.
pub const DEFAULT_MAX_TRAILER_BYTES: usize = 16 * 1024;

/// `Number.MAX_SAFE_INTEGER`.
const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;

fn type_error(message: &Malformed) -> cxx::KjError {
    cxx::KjError::new(
        cxx::KjExceptionType::Failed,
        format!("jsg.TypeError: {message}"),
    )
}

fn range_error(message: &str) -> cxx::KjError {
    cxx::KjError::new(
        cxx::KjExceptionType::Failed,
        format!("jsg.RangeError: {message}"),
    )
}

/// Converts an option that must be a non-negative safe integer.
fn to_length(present: bool, value: f64, name: &str) -> Result<Option<u64>, cxx::KjError> {
    if !present {
        return Ok(None);
    }
    if !(0.0..=MAX_SAFE_INTEGER).contains(&value) || value.fract() != 0.0 {
        return Err(range_error(&format!(
            "{name} must be a non-negative integer no greater than Number.MAX_SAFE_INTEGER."
        )));
    }
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "the value is a non-negative integer of at most 2^53 - 1"
    )]
    Ok(Some(value as u64))
}

/// Drives a [`decoder::Decoder`] for C++. Once the input is invalid it fails: at once, or, when
/// draining, when the input ends, having read and discarded the rest of it.
pub struct Decoder {
    inner: decoder::Decoder,
    decoded_length: Option<u64>,
    drain_on_error: bool,
    error: Option<Malformed>,
    output: Vec<u8>,
}

/// Validates `options` and creates a decoder.
///
/// # Errors
///
/// Fails with a `RangeError` if a length is not a non-negative safe integer.
pub fn new_decoder(options: ffi::DecoderOptions) -> Result<Box<Decoder>, cxx::KjError> {
    let decoded_length = to_length(
        options.has_decoded_length,
        options.decoded_length,
        "decodedLength",
    )?;
    let max_trailer_bytes = to_length(
        options.has_max_trailer_bytes,
        options.max_trailer_bytes,
        "maxTrailerBytes",
    )?
    .map_or(DEFAULT_MAX_TRAILER_BYTES, |max| {
        usize::try_from(max).unwrap_or(usize::MAX)
    });
    Ok(Box::new(Decoder {
        inner: decoder::Decoder::new(Options {
            decoded_length,
            allowed_trailers: options
                .has_allowed_trailers
                .then_some(options.allowed_trailers),
            max_trailer_bytes,
        }),
        decoded_length,
        drain_on_error: options.drain_on_error,
        error: None,
        output: Vec::new(),
    }))
}

impl Decoder {
    /// Decodes `input`.
    ///
    /// # Errors
    ///
    /// Fails with a `TypeError` if the input is invalid, unless the decoder drains.
    pub fn push(&mut self, input: &[u8]) -> Result<(), cxx::KjError> {
        self.output = Vec::new();
        if let Some(error) = &self.error {
            // Draining: the input is read and discarded until it ends.
            return if self.drain_on_error {
                Ok(())
            } else {
                Err(type_error(error))
            };
        }
        // Decoded data is never longer than its framing.
        let mut output = Vec::with_capacity(input.len());
        let result = self.inner.push(input, &mut output);
        self.output = output;
        match result {
            Ok(()) => Ok(()),
            Err(error) => {
                let failure = type_error(&error);
                self.error = Some(error);
                if self.drain_on_error {
                    Ok(())
                } else {
                    Err(failure)
                }
            }
        }
    }

    /// Finishes the body.
    ///
    /// # Errors
    ///
    /// Fails with a `TypeError` if the input was invalid or incomplete.
    pub fn end(&mut self) -> Result<Vec<ffi::TrailerField>, cxx::KjError> {
        if let Some(error) = &self.error {
            return Err(type_error(error));
        }
        let fields = self.inner.end().map_err(|error| type_error(&error))?;
        Ok(fields
            .into_iter()
            .map(|field| ffi::TrailerField {
                name: field.name,
                value: field.value,
            })
            .collect())
    }

    pub fn take_output(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.output)
    }

    #[must_use]
    pub fn decoded_length(&self) -> kj_rs::KjMaybe<u64> {
        self.decoded_length.into()
    }

    #[must_use]
    pub const fn retained_bytes(&self) -> usize {
        self.inner.retained_bytes()
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
