// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! WHATWG Encoding Standard legacy decoders via `encoding_rs`.
//!
//! Exposes a streaming decoder to C++ via CXX bridge. All legacy encodings
//! (CJK multi-byte, single-byte windows-1252, and x-user-defined) are handled
//! by a single opaque `Decoder` type backed by `encoding_rs::Decoder`.
//!
//! The output buffer is owned by the `Decoder` and reused across calls to
//! avoid repeated heap allocations. C++ reads the decoded UTF-16 data via
//! the pointer and length returned in `DecodeResult`.

#[cxx::bridge(namespace = "workerd::rust::encoding")]
mod ffi {
    /// Legacy encoding types supported by the Rust decoder.
    /// Shared between C++ and Rust.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(u16)]
    enum Encoding {
        Big5,
        EucJp,
        EucKr,
        Gb18030,
        Gbk,
        Iso2022Jp,
        ShiftJis,
        Windows1252,
        XUserDefined,
    }

    /// Result of a decode operation. The output slice borrows the
    /// decoder's internal buffer and is valid until the next `decode` or
    /// `reset` call.
    struct DecodeResult<'a> {
        /// UTF-16 code units decoded from the input, borrowing the
        /// decoder's reusable output buffer.
        output: &'a [u16],
        /// True if a fatal decoding error was encountered. Only meaningful
        /// when the caller requested fatal mode — in replacement mode errors
        /// are silently replaced with U+FFFD and this flag is not set.
        had_error: bool,
    }

    struct DecodeOptions {
        flush: bool,
        fatal: bool,
    }

    extern "Rust" {
        type Decoder;

        /// Create a new streaming decoder for the given encoding.
        // CXX bridge requires Box for opaque types.
        #[expect(clippy::unnecessary_box_returns)]
        fn new_decoder(encoding: Encoding) -> Box<Decoder>;

        /// Decode a chunk of bytes. The decoded UTF-16 output is stored in
        /// the decoder's internal buffer; the returned `DecodeResult`
        /// borrows that buffer. Set `flush` to true on the final chunk.
        /// When `fatal` is true and an error is encountered, `had_error`
        /// is set and the output may be incomplete.
        unsafe fn decode<'a>(
            decoder: &'a mut Decoder,
            input: &[u8],
            options: &DecodeOptions,
        ) -> DecodeResult<'a>;

        /// Reset the decoder to its initial state (for explicit reset calls).
        fn reset(decoder: &mut Decoder);
    }
}

/// Opaque decoder state exposed to C++ via `Box<Decoder>`.
pub struct Decoder {
    encoding: &'static encoding_rs::Encoding,
    inner: encoding_rs::Decoder,
    /// Reusable output buffer — kept across calls to avoid allocation.
    output: Vec<u16>,
    /// Set after a flush decode; checked at the start of the next decode
    /// to lazily reconstruct the inner decoder.
    needs_reset: bool,
}

/// Map a CXX-shared `Encoding` variant to the corresponding
/// `encoding_rs` static.
fn to_encoding(enc: ffi::Encoding) -> &'static encoding_rs::Encoding {
    match enc {
        ffi::Encoding::Big5 => encoding_rs::BIG5,
        ffi::Encoding::EucJp => encoding_rs::EUC_JP,
        ffi::Encoding::EucKr => encoding_rs::EUC_KR,
        ffi::Encoding::Gb18030 => encoding_rs::GB18030,
        ffi::Encoding::Gbk => encoding_rs::GBK,
        ffi::Encoding::Iso2022Jp => encoding_rs::ISO_2022_JP,
        ffi::Encoding::ShiftJis => encoding_rs::SHIFT_JIS,
        ffi::Encoding::Windows1252 => encoding_rs::WINDOWS_1252,
        ffi::Encoding::XUserDefined => encoding_rs::X_USER_DEFINED,
        _ => unreachable!(),
    }
}

pub fn new_decoder(encoding: ffi::Encoding) -> Box<Decoder> {
    let encoding = to_encoding(encoding);
    Box::new(Decoder {
        inner: encoding.new_decoder_without_bom_handling(),
        encoding,
        output: Vec::new(),
        needs_reset: false,
    })
}

pub fn decode<'a>(
    state: &'a mut Decoder,
    input: &[u8],
    options: &ffi::DecodeOptions,
) -> ffi::DecodeResult<'a> {
    // Lazy reset: reconstruct the inner decoder only when a previous flush
    // marked it as needed, avoiding the cost on one-shot decodes where the
    // decoder is never reused.
    if state.needs_reset {
        state.inner = state.encoding.new_decoder_without_bom_handling();
        state.needs_reset = false;
    }

    // Reuse the output buffer — clear length but keep the allocation.
    state.output.clear();
    let max_len = state
        .inner
        .max_utf16_buffer_length(input.len())
        .unwrap_or(input.len() + 4);
    state.output.resize(max_len, 0);

    let mut total_read = 0usize;
    let mut total_written = 0usize;

    if options.fatal {
        loop {
            let (result, read, written) = state.inner.decode_to_utf16_without_replacement(
                &input[total_read..],
                &mut state.output[total_written..],
                options.flush,
            );
            total_read += read;
            total_written += written;

            match result {
                encoding_rs::DecoderResult::InputEmpty => break,
                encoding_rs::DecoderResult::OutputFull => {
                    state.output.resize(state.output.len() * 2, 0);
                }
                encoding_rs::DecoderResult::Malformed(_, _) => {
                    // Reset immediately on fatal error so the decoder is
                    // ready for a fresh sequence if reused.
                    state.inner = state.encoding.new_decoder_without_bom_handling();
                    state.output.truncate(total_written);
                    return ffi::DecodeResult {
                        output: &state.output,
                        had_error: true,
                    };
                }
            }
        }
    } else {
        loop {
            let (result, read, written, _had_errors) = state.inner.decode_to_utf16(
                &input[total_read..],
                &mut state.output[total_written..],
                options.flush,
            );
            total_read += read;
            total_written += written;

            match result {
                encoding_rs::CoderResult::InputEmpty => break,
                encoding_rs::CoderResult::OutputFull => {
                    state.output.resize(state.output.len() * 2, 0);
                }
            }
        }
    }

    state.output.truncate(total_written);

    if options.flush {
        // Defer the actual reset to the next decode() call.
        state.needs_reset = true;
    }

    ffi::DecodeResult {
        output: &state.output,
        had_error: false,
    }
}

pub fn reset(state: &mut Decoder) {
    state.inner = state.encoding.new_decoder_without_bom_handling();
    state.needs_reset = false;
    // Intentionally keep state.output — preserves the allocation for reuse.
    // The buffer can grow up to ~2× the largest input chunk (due to UTF-16
    // expansion and the doubling strategy in decode()) and stays at that high-
    // water mark. This is acceptable because the Decoder is owned by a JS
    // TextDecoder object and is GC'd with it, so the buffer lifetime is
    // bounded by the object's reachability.
}
