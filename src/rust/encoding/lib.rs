// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! WHATWG Encoding Standard legacy decoders via `encoding_rs`.
//!
//! Exposes a streaming decoder to C++ via CXX bridge. All legacy encodings
//! (CJK multi-byte, single-byte windows-1252, and x-user-defined) are handled
//! by a single opaque `Decoder` type backed by `encoding_rs::Decoder`.

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

    /// Result of a decode operation.
    struct DecodeResult {
        /// UTF-16 output.
        output: Vec<u16>,
        /// True if a fatal decoding error was encountered. Only meaningful
        /// when the caller requested fatal mode — in replacement mode errors
        /// are silently replaced with U+FFFD and this flag is not set.
        had_error: bool,
    }

    extern "Rust" {
        type Decoder;

        /// Create a new streaming decoder for the given encoding.
        // CXX bridge requires Box for opaque types.
        #[expect(clippy::unnecessary_box_returns)]
        fn new_decoder(encoding: Encoding) -> Box<Decoder>;

        /// Decode a chunk of bytes. Set `flush` to true on the final chunk.
        /// When `fatal` is true and an error is encountered, `had_error` is
        /// set and the output may be incomplete.
        fn decode(decoder: &mut Decoder, input: &[u8], flush: bool, fatal: bool) -> DecodeResult;

        /// Reset the decoder to its initial state.
        fn reset(decoder: &mut Decoder);
    }
}

/// Opaque decoder state exposed to C++ via `Box<Decoder>`.
pub struct Decoder {
    encoding: &'static encoding_rs::Encoding,
    inner: encoding_rs::Decoder,
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
    })
}

pub fn decode(state: &mut Decoder, input: &[u8], flush: bool, fatal: bool) -> ffi::DecodeResult {
    let max_len = state
        .inner
        .max_utf16_buffer_length(input.len())
        .unwrap_or(input.len() + 4);
    let mut output = vec![0u16; max_len];
    let mut total_read = 0usize;
    let mut total_written = 0usize;

    if fatal {
        loop {
            let (result, read, written) = state.inner.decode_to_utf16_without_replacement(
                &input[total_read..],
                &mut output[total_written..],
                flush,
            );
            total_read += read;
            total_written += written;

            match result {
                encoding_rs::DecoderResult::InputEmpty => break,
                encoding_rs::DecoderResult::OutputFull => {
                    output.resize(output.len() * 2, 0);
                }
                encoding_rs::DecoderResult::Malformed(_, _) => {
                    state.inner = state.encoding.new_decoder_without_bom_handling();
                    output.truncate(total_written);
                    return ffi::DecodeResult {
                        output,
                        had_error: true,
                    };
                }
            }
        }
    } else {
        loop {
            let (result, read, written, _had_errors) = state.inner.decode_to_utf16(
                &input[total_read..],
                &mut output[total_written..],
                flush,
            );
            total_read += read;
            total_written += written;

            match result {
                encoding_rs::CoderResult::InputEmpty => break,
                encoding_rs::CoderResult::OutputFull => {
                    output.resize(output.len() * 2, 0);
                }
            }
        }
    }

    output.truncate(total_written);
    ffi::DecodeResult {
        output,
        had_error: false,
    }
}

pub fn reset(state: &mut Decoder) {
    state.inner = state.encoding.new_decoder_without_bom_handling();
}
