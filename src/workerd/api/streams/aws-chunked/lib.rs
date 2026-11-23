// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Decodes the aws-chunked content encoding of S3 streaming uploads, for C++'s
//! `AwsChunkedDecoderStream` (`src/workerd/api/streams/aws-chunked.h`).
//!
//! The decoding is the `s3s-chunked` crate's: an asynchronous stream over the body's fragments.
//! Its state machine only advances when it is polled and never waits on anything but its input,
//! so [`Decoder`] drives it synchronously: each write is queued as the next fragment, and the
//! stream is polled until it needs another. Once the input ends, the queue reports the end, and
//! the last poll publishes the trailer fields.
//!
//! C++ drives the [`Decoder`] from a `SyncTransformStage`. Errors are `TypeError`s and
//! `RangeError`s, described as JSG expects (`jsg.TypeError: ...`) so they reach JavaScript as
//! such.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use bytes::Bytes;
use futures::Stream;
use s3s_chunked::ChunkedStream;
use s3s_chunked::Error;
use s3s_chunked::Limits;
use s3s_chunked::Sha256Sum;
use s3s_chunked::SignContext;
use s3s_chunked::StdError;
use s3s_sigv4::AmzDate;

#[cxx::bridge(namespace = "workerd::api::aws_chunked")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
#[expect(
    clippy::needless_lifetimes,
    reason = "cxx requires explicit lifetimes on a result that borrows"
)]
mod ffi {
    /// A trailer field.
    #[derive(Debug)]
    struct TrailerField {
        name: String,
        value: String,
    }

    /// What a signed body is verified with: the credentials and scope of the request's SigV4
    /// signature, and that signature itself, which seeds the chain of chunk signatures.
    struct SigningOptions {
        secret_access_key: String,
        /// The request's `x-amz-date`, such as `20130524T000000Z`.
        amz_date: String,
        region: String,
        service: String,
        /// The request's signature, as 64 hex digits.
        seed_signature: String,
    }

    /// The decoder's options, as JavaScript passed them. Each `has_` flag says whether the
    /// value after it was given.
    struct DecoderOptions {
        decoded_length: f64,
        has_allowed_trailers: bool,
        allowed_trailers: Vec<String>,
        has_max_trailer_bytes: bool,
        max_trailer_bytes: f64,
        drain_on_error: bool,
        has_signing: bool,
        signing: SigningOptions,
    }

    extern "Rust" {
        type Decoder;
        /// A piece of decoded data.
        type Output;

        /// Validates `options` and creates a decoder.
        fn new_decoder(options: DecoderOptions) -> Result<Box<Decoder>>;

        /// Decodes `input`. The decoded data is taken with `pop_output()`, also after a failure,
        /// when it is the data decoded before the error.
        fn push(self: &mut Decoder, input: &[u8]) -> Result<()>;

        /// Finishes the body, returning the trailer fields that are allowed.
        fn end(self: &mut Decoder) -> Result<Vec<TrailerField>>;

        /// Whether decoded data waits to be taken.
        fn has_output(self: &Decoder) -> bool;

        /// Takes the next piece of decoded data, or an empty one if none waits.
        fn pop_output(self: &mut Decoder) -> Box<Output>;

        /// The data, valid while the `Output` lives. Unsafe only in that cxx requires it of a
        /// function whose result borrows from its arguments.
        unsafe fn data<'a>(self: &'a Output) -> &'a [u8];

        /// The decoded length.
        fn decoded_length(self: &Decoder) -> u64;

        /// The bytes the decoder holds between calls, for memory accounting.
        fn retained_bytes(self: &Decoder) -> usize;
    }
}

/// `Number.MAX_SAFE_INTEGER`.
const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;

fn type_error(message: &str) -> cxx::KjError {
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
fn to_length(value: f64, name: &str) -> Result<usize, cxx::KjError> {
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
    let value = value as u64;
    usize::try_from(value).map_err(|_| range_error(&format!("{name} is too large.")))
}

/// The body's fragments as `ChunkedStream` reads them: the writes not yet decoded, then the end
/// of the input once `Decoder::end()` has been called. Until then an empty queue is `Pending`,
/// which returns control from the stream, since nothing else would wake it.
#[derive(Default)]
struct Input {
    fragments: VecDeque<Bytes>,
    ended: bool,
}

#[derive(Clone, Default)]
struct InputStream(Rc<RefCell<Input>>);

impl Stream for InputStream {
    type Item = Result<Bytes, StdError>;

    fn poll_next(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let mut input = self.0.borrow_mut();
        match input.fragments.pop_front() {
            Some(fragment) => Poll::Ready(Some(Ok(fragment))),
            None if input.ended => Poll::Ready(None),
            None => Poll::Pending,
        }
    }
}

/// Describes a decoding error. `produced` is the number of bytes decoded before it.
fn describe(error: &Error, decoded_length: usize, produced: usize) -> String {
    match error {
        Error::FormatError => "Malformed aws-chunked body.".to_owned(),
        Error::Incomplete if produced < decoded_length => format!(
            "The aws-chunked body ended after {produced} of its {decoded_length} decoded bytes."
        ),
        Error::Incomplete => "The aws-chunked body ended before its final chunk.".to_owned(),
        Error::SignatureMismatch => {
            "An aws-chunked chunk or trailer signature does not match.".to_owned()
        }
        Error::LengthMismatch => format!(
            "The aws-chunked body decodes to more than its decoded length ({decoded_length} \
             bytes)."
        ),
        Error::TrailersMissing | Error::TrailersEmpty => {
            "The aws-chunked body has no trailer.".to_owned()
        }
        Error::ChunkMetaTooLarge(_, max) => {
            format!("An aws-chunked chunk size line is longer than {max} bytes.")
        }
        Error::ChunkDataTooLarge(_, max) => {
            format!("A signed aws-chunked chunk is larger than {max} bytes.")
        }
        Error::TrailersTooLarge(_, max) => {
            format!("The aws-chunked trailer is longer than {max} bytes.")
        }
        Error::TooManyTrailerHeaders(_, max) => {
            format!("The aws-chunked trailer has more than {max} fields.")
        }
        // The input never fails.
        Error::Underlying(error) => format!("The aws-chunked body could not be read: {error}."),
    }
}

/// A piece of decoded data. It is a view of the buffer the input was copied into, so passing it
/// to C++ copies nothing.
pub struct Output(Bytes);

impl Output {
    #[must_use]
    pub fn data(&self) -> &[u8] {
        &self.0
    }
}

/// Drives a `ChunkedStream` for C++. Once the input is invalid it fails: at once, or, when
/// draining, when the input ends, having read and discarded the rest of it.
pub struct Decoder {
    stream: ChunkedStream<InputStream>,
    input: InputStream,
    decoded_length: usize,
    allowed_trailers: Option<Vec<String>>,
    drain_on_error: bool,
    error: Option<String>,
    output: VecDeque<Bytes>,
    /// The input bytes passed to the stream since it last produced data, which bounds what it
    /// buffers: a signed chunk until its signature is verified, a partial size line, or the
    /// trailer section.
    taken_since_output: usize,
}

fn signed_stream(
    input: InputStream,
    signing: ffi::SigningOptions,
    decoded_length: usize,
    limits: Limits,
) -> Result<ChunkedStream<InputStream>, cxx::KjError> {
    let amz_date = AmzDate::parse(&signing.amz_date).map_err(|_| {
        type_error("signing.amzDate must be an x-amz-date such as 20130524T000000Z.")
    })?;
    let seed_signature = Sha256Sum::from_hex(&signing.seed_signature)
        .ok_or_else(|| type_error("signing.seedSignature must be 64 hex digits."))?;
    let context = SignContext::new(
        amz_date,
        signing.region.into_boxed_str(),
        signing.service.into_boxed_str(),
        signing.secret_access_key.as_bytes(),
    );
    Ok(ChunkedStream::signed(
        input,
        context,
        seed_signature,
        decoded_length,
        limits,
    ))
}

/// Validates `options` and creates a decoder.
///
/// # Errors
///
/// Fails with a `RangeError` if a length is not a non-negative safe integer, and with a
/// `TypeError` if a signing option is malformed.
pub fn new_decoder(options: ffi::DecoderOptions) -> Result<Box<Decoder>, cxx::KjError> {
    let decoded_length = to_length(options.decoded_length, "decodedLength")?;
    let mut limits = Limits::default();
    if options.has_max_trailer_bytes {
        limits.max_trailers_size = to_length(options.max_trailer_bytes, "maxTrailerBytes")?;
    }
    let input = InputStream::default();
    let stream = if options.has_signing {
        signed_stream(input.clone(), options.signing, decoded_length, limits)?
    } else {
        ChunkedStream::unsigned(input.clone(), decoded_length, limits)
    };
    Ok(Box::new(Decoder {
        stream,
        input,
        decoded_length,
        allowed_trailers: options
            .has_allowed_trailers
            .then_some(options.allowed_trailers),
        drain_on_error: options.drain_on_error,
        error: None,
        output: VecDeque::new(),
        taken_since_output: 0,
    }))
}

impl Decoder {
    /// Polls the stream until it needs more input or has finished, keeping the data it produces.
    fn poll(&mut self) -> Result<(), String> {
        let mut context = Context::from_waker(Waker::noop());
        loop {
            match Pin::new(&mut self.stream).poll_next(&mut context) {
                Poll::Pending | Poll::Ready(None) => return Ok(()),
                Poll::Ready(Some(Ok(data))) => {
                    self.taken_since_output = 0;
                    if !data.is_empty() {
                        self.output.push_back(data);
                    }
                }
                Poll::Ready(Some(Err(error))) => {
                    let produced = self.decoded_length - self.stream.exact_remaining_length();
                    return Err(describe(&error, self.decoded_length, produced));
                }
            }
        }
    }

    /// Decodes `input`.
    ///
    /// # Errors
    ///
    /// Fails with a `TypeError` if the input is invalid, unless the decoder drains.
    pub fn push(&mut self, input: &[u8]) -> Result<(), cxx::KjError> {
        if let Some(error) = &self.error {
            // Draining: the input is read and discarded until it ends.
            return if self.drain_on_error {
                Ok(())
            } else {
                Err(type_error(error))
            };
        }
        // The stream counts a run of empty fragments as malformed, and an empty write adds
        // nothing.
        if input.is_empty() {
            return Ok(());
        }
        self.taken_since_output = self.taken_since_output.saturating_add(input.len());
        self.input
            .0
            .borrow_mut()
            .fragments
            .push_back(Bytes::copy_from_slice(input));
        match self.poll() {
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
        self.input.0.borrow_mut().ended = true;
        let trailers = self.stream.trailer_handle();
        self.poll().map_err(|error| type_error(&error))?;
        // The stream publishes the trailer section once it has accepted it. A body that ends
        // without one has no fields.
        let Some(headers) = trailers.take() else {
            return Ok(Vec::new());
        };
        // The stream accepts control characters in field values, which `Headers` does not.
        let is_control = |c: u8| c != b'\t' && (c < 0x20 || c == 0x7f);
        if headers
            .values()
            .any(|value| value.as_bytes().iter().copied().any(is_control))
        {
            return Err(type_error("Malformed aws-chunked trailer field value."));
        }
        Ok(headers
            .iter()
            .filter(|(name, _)| self.is_allowed(name.as_str()))
            .map(|(name, value)| ffi::TrailerField {
                name: name.as_str().to_owned(),
                value: String::from_utf8_lossy(value.as_bytes()).into_owned(),
            })
            .collect())
    }

    fn is_allowed(&self, name: &str) -> bool {
        self.allowed_trailers
            .as_ref()
            .is_none_or(|allowed| allowed.iter().any(|entry| entry.eq_ignore_ascii_case(name)))
    }

    #[must_use]
    pub fn has_output(&self) -> bool {
        !self.output.is_empty()
    }

    pub fn pop_output(&mut self) -> Box<Output> {
        Box::new(Output(self.output.pop_front().unwrap_or_default()))
    }

    #[must_use]
    pub fn decoded_length(&self) -> u64 {
        u64::try_from(self.decoded_length).unwrap_or(u64::MAX)
    }

    /// Approximately the bytes the stream holds between calls: those it has been given since it
    /// last produced data. Part of the write in which it did so may be held too.
    #[must_use]
    pub const fn retained_bytes(&self) -> usize {
        self.taken_since_output
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
