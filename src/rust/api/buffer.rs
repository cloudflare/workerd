// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust implementation of `node-internal:buffer` (`BufferUtil`).
//!
//! Mirrors the C++ `BufferUtil` class in `src/workerd/api/node/buffer.h`.
//! Uses `simdutf` for SIMD-accelerated validation and transcoding.

use jsg::jsg_require;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;
use jsg_macros::jsg_struct;

/// Matches C++ `kj::OneOf<jsg::JsString, jsg::JsUint8Array>` used by
/// `indexOf` and `fillImpl` — the JS caller can pass either a string
/// (which is decoded using the encoding parameter) or a `Uint8Array`.
#[jsg_oneof]
#[derive(Debug, Clone)]
pub enum StringOrBuffer {
    String(String),
    Buffer(Vec<u8>),
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Encoding {
    Ascii = 0,
    Latin1 = 1,
    Utf8 = 2,
    Utf16le = 3,
    Base64 = 4,
    Base64url = 5,
    Hex = 6,
}

impl Encoding {
    fn from_value(value: u8) -> Result<Self, jsg::Error> {
        match value {
            0 => Ok(Self::Ascii),
            1 => Ok(Self::Latin1),
            2 => Ok(Self::Utf8),
            3 => Ok(Self::Utf16le),
            4 => Ok(Self::Base64),
            5 => Ok(Self::Base64url),
            6 => Ok(Self::Hex),
            _ => Err(jsg::Error::new_range_error(format!(
                "Unknown encoding: {value}"
            ))),
        }
    }
}

/// Options for `BufferUtil::compare` sub-range comparison.
/// Matches C++ `CompareOptions` struct.
#[jsg_struct]
pub struct CompareOptions {
    pub a_start: Option<jsg::Number>,
    pub a_end: Option<jsg::Number>,
    pub b_start: Option<jsg::Number>,
    pub b_end: Option<jsg::Number>,
}

const HEX_CHARS: &[u8; 16] = b"0123456789abcdef";
const INCOMPLETE_START: usize = 0;
const MISSING_BYTES_FIELD: usize = 4;
const BUFFERED_BYTES_FIELD: usize = 5;
const ENCODING_FIELD: usize = 6;
const K_SIZE: usize = 7;

fn hex_val(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

fn hex_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len() * 2);
    for &b in data {
        out.push(HEX_CHARS[(b >> 4) as usize] as char);
        out.push(HEX_CHARS[(b & 0xf) as usize] as char);
    }
    out
}

/// Decodes hex, truncating at the first invalid hex pair (Node.js behavior).
/// In strict mode, returns an error on invalid input instead of truncating.
fn hex_decode(src: &[u8], strict: bool) -> Result<Vec<u8>, jsg::Error> {
    let len = src.len() & !1;
    if len != src.len() && strict {
        return Err(jsg::Error::new_type_error("The text is not valid hex"));
    }
    let mut out = Vec::with_capacity(len / 2);
    for i in (0..len).step_by(2) {
        let Some(hi) = hex_val(src[i]) else {
            if strict {
                return Err(jsg::Error::new_type_error("The text is not valid hex"));
            }
            break;
        };
        let Some(lo) = hex_val(src[i + 1]) else {
            if strict {
                return Err(jsg::Error::new_type_error("The text is not valid hex"));
            }
            break;
        };
        out.push((hi << 4) | lo);
    }
    Ok(out)
}

/// Convert raw bytes to a String using the given encoding. This mirrors
/// the C++ `toStringImpl` used internally by `decode` / `flush`.
#[expect(
    clippy::unnecessary_wraps,
    reason = "callers expect Result for consistency with decode/flush error paths"
)]
fn to_string_for_encoding(data: &[u8], enc: Encoding) -> Result<String, jsg::Error> {
    if data.is_empty() {
        return Ok(String::new());
    }
    Ok(match enc {
        Encoding::Ascii => data.iter().map(|&b| (b & 0x7f) as char).collect(),
        Encoding::Latin1 => {
            // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
            unsafe {
                let latin1_len = simdutf::utf8_length_from_latin1(data);
                let mut out = vec![0u8; latin1_len];
                let _ =
                    simdutf::convert_latin1_to_utf8(data.as_ptr(), data.len(), out.as_mut_ptr());
                // SAFETY: simdutf produces valid UTF-8.
                String::from_utf8_unchecked(out)
            }
        }
        Encoding::Utf8 => String::from_utf8_lossy(data).into_owned(),
        Encoding::Utf16le => {
            // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
            unsafe {
                let u16buf: Vec<u16> = data
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .collect();
                let utf8_len = simdutf::utf8_length_from_utf16le(&u16buf);
                let mut out = vec![0u8; utf8_len];
                let written = simdutf::convert_utf16le_to_utf8(
                    u16buf.as_ptr(),
                    u16buf.len(),
                    out.as_mut_ptr(),
                );
                out.truncate(written);
                // SAFETY: simdutf produces valid UTF-8.
                String::from_utf8_unchecked(out)
            }
        }
        Encoding::Base64 | Encoding::Base64url => {
            let b64_opts = match enc {
                Encoding::Base64url => simdutf::Base64Options::Url,
                _ => simdutf::Base64Options::Default,
            };
            // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
            unsafe {
                let b64_len = simdutf::base64_length_from_binary(data.len(), b64_opts);
                let mut out = vec![0u8; b64_len];
                let _ = simdutf::binary_to_base64(
                    data.as_ptr(),
                    data.len(),
                    out.as_mut_ptr(),
                    b64_opts,
                );
                // SAFETY: base64 output is valid ASCII.
                String::from_utf8_unchecked(out)
            }
        }
        Encoding::Hex => hex_encode(data),
    })
}

#[jsg_resource]
pub struct BufferUtil;

#[jsg_resource]
impl BufferUtil {
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self)
    }

    // These constants are exposed to JavaScript and must be kept in sync with the
    // `Encoding` enum discriminants above. If the enum changes, update these too.
    #[jsg_static_constant]
    pub const ASCII: u8 = 0;
    #[jsg_static_constant]
    pub const LATIN1: u8 = 1;
    #[jsg_static_constant]
    pub const UTF8: u8 = 2;
    #[jsg_static_constant]
    pub const UTF16LE: u8 = 3;
    #[jsg_static_constant]
    pub const BASE64: u8 = 4;
    #[jsg_static_constant]
    pub const BASE64URL: u8 = 5;
    #[jsg_static_constant]
    pub const HEX: u8 = 6;

    #[jsg_method]
    pub fn is_ascii(&self, bytes: jsg::v8::Local<jsg::v8::Uint8Array>) -> bool {
        let data = bytes.as_slice();
        if data.is_empty() {
            return true;
        }
        simdutf::validate_ascii(data)
    }

    #[jsg_method]
    pub fn is_utf8(&self, bytes: jsg::v8::Local<jsg::v8::Uint8Array>) -> bool {
        let data = bytes.as_slice();
        if data.is_empty() {
            return true;
        }
        simdutf::validate_utf8(data)
    }

    #[jsg_method]
    pub fn byte_length(&self, str: String) -> jsg::Number {
        // The TS caller handles all non-UTF-8 encodings itself; this method is only
        // invoked for UTF-8 (the default case), matching C++ `str.utf8Length(js)`.
        #[expect(clippy::cast_precision_loss, reason = "buffer lengths < 2^53")]
        jsg::Number::new(str.len() as f64)
    }

    #[jsg_method]
    pub fn swap(
        &self,
        mut buffer: jsg::v8::Local<jsg::v8::Uint8Array>,
        size: jsg::Number,
    ) -> Result<(), jsg::Error> {
        if buffer.len() <= 1 {
            return Ok(());
        }
        let size = size.value() as i32;
        let cs = match size {
            16 => 2,
            32 => 4,
            64 => 8,
            _ => {
                jsg::jsg_fail_require!(Error, "Unreachable");
            }
        };
        let data = buffer.as_mut_slice();
        jsg_require!(data.len() % cs == 0, Error, "Swap bytes failed");
        for chunk in data.chunks_exact_mut(cs) {
            chunk.reverse();
        }
        Ok(())
    }

    #[jsg_method]
    pub fn compare(
        &self,
        one: jsg::v8::Local<jsg::v8::Uint8Array>,
        two: jsg::v8::Local<jsg::v8::Uint8Array>,
        maybe_options: Option<CompareOptions>,
    ) -> jsg::Number {
        let slice_one = one.as_slice();
        let slice_two = two.as_slice();

        // Apply sub-range options (matching C++ CompareOptions).
        let (ptr_one, ptr_two) = if let Some(opts) = maybe_options {
            #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
            let a_end = opts.a_end.map_or(slice_one.len(), |n| {
                (n.value().max(0.0) as usize).min(slice_one.len())
            });
            #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
            let a_start = opts
                .a_start
                .map_or(0, |n| (n.value().max(0.0) as usize).min(a_end));
            #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
            let b_end = opts.b_end.map_or(slice_two.len(), |n| {
                (n.value().max(0.0) as usize).min(slice_two.len())
            });
            #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
            let b_start = opts
                .b_start
                .map_or(0, |n| (n.value().max(0.0) as usize).min(b_end));
            (&slice_one[a_start..a_end], &slice_two[b_start..b_end])
        } else {
            (slice_one, slice_two)
        };

        let to_compare = ptr_one.len().min(ptr_two.len());
        let result = if to_compare > 0 {
            ptr_one[..to_compare].cmp(&ptr_two[..to_compare])
        } else {
            std::cmp::Ordering::Equal
        };

        match result {
            std::cmp::Ordering::Equal => {
                jsg::Number::new(match ptr_one.len().cmp(&ptr_two.len()) {
                    std::cmp::Ordering::Greater => 1.0,
                    std::cmp::Ordering::Less => -1.0,
                    std::cmp::Ordering::Equal => 0.0,
                })
            }
            std::cmp::Ordering::Greater => jsg::Number::new(1.0),
            std::cmp::Ordering::Less => jsg::Number::new(-1.0),
        }
    }

    #[jsg_method]
    pub fn concat(&self, list: Vec<Vec<u8>>, length: jsg::Number) -> Result<Vec<u8>, jsg::Error> {
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let length = length.value().max(0.0) as usize;
        jsg_require!(
            length <= 2_147_483_647,
            RangeError,
            "The length is too large"
        );
        let mut r = Vec::with_capacity(length);
        for buf in &list {
            let rem = length.saturating_sub(r.len());
            if rem == 0 {
                break;
            }
            r.extend_from_slice(&buf[..buf.len().min(rem)]);
        }
        r.resize(length, 0);
        Ok(r)
    }

    #[jsg_method]
    pub fn to_string(
        &self,
        data: &[u8],
        start: jsg::Number,
        end: jsg::Number,
        encoding: jsg::Number,
    ) -> Result<String, jsg::Error> {
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let s = (start.value().max(0.0) as usize).min(data.len());
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let e = (end.value().max(0.0) as usize).min(data.len());
        #[expect(clippy::cast_sign_loss, reason = "validated by Encoding::from_value")]
        let enc_val = encoding.value().max(0.0) as u8;
        let enc = Encoding::from_value(enc_val)?;
        if s >= e {
            return Ok(String::new());
        }
        to_string_for_encoding(&data[s..e], enc)
    }

    #[jsg_method]
    pub fn decode_string(
        &self,
        string: String,
        encoding: jsg::Number,
    ) -> Result<Vec<u8>, jsg::Error> {
        #[expect(clippy::cast_sign_loss, reason = "validated by Encoding::from_value")]
        let enc_val = encoding.value().max(0.0) as u8;
        let enc = Encoding::from_value(enc_val)?;
        let src = string.as_bytes();
        Ok(match enc {
            // Convert from UTF-8 chars (not raw bytes) to get the correct Unicode code points.
            // ASCII masks to 7 bits; Latin1 takes the full low byte.
            Encoding::Ascii => string.chars().map(|c| (c as u8) & 0x7f).collect(),
            Encoding::Latin1 => string.chars().map(|c| c as u8).collect(),
            Encoding::Utf8 => src.to_vec(),
            Encoding::Utf16le => {
                let u16_len = simdutf::utf16_length_from_utf8(src);
                let mut u16buf = vec![0u16; u16_len];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                unsafe {
                    let _ = simdutf::convert_utf8_to_utf16le(
                        src.as_ptr(),
                        src.len(),
                        u16buf.as_mut_ptr(),
                    );
                }
                u16buf
                    .iter()
                    .flat_map(|u| u.to_le_bytes())
                    .collect::<Vec<u8>>()
            }
            Encoding::Base64 | Encoding::Base64url => {
                // Node.js uses nbytes::Base64Decode which is very lenient — it
                // ignores invalid characters and decodes whatever valid base64
                // chars are present.
                nbytes::base64_decode(src)
            }
            Encoding::Hex => hex_decode(src, false)?,
        })
    }

    #[jsg_method]
    pub fn write(
        &self,
        mut buffer: jsg::v8::Local<jsg::v8::Uint8Array>,
        string: String,
        offset: jsg::Number,
        length: jsg::Number,
        encoding: jsg::Number,
    ) -> Result<jsg::Number, jsg::Error> {
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let offset = offset.value().max(0.0) as usize;
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let length = length.value().max(0.0) as usize;
        #[expect(clippy::cast_sign_loss, reason = "validated by Encoding::from_value")]
        let enc = Encoding::from_value(encoding.value().max(0.0) as u8)?;
        let encoded = self.decode_string(string, encoding)?;
        let data = buffer.as_mut_slice();
        let mut written = encoded
            .len()
            .min(data.len().saturating_sub(offset).min(length));
        // UTF-16LE uses 2-byte code units — truncate to a whole number of code units
        // so we never write half a character.
        if enc == Encoding::Utf16le {
            written &= !1;
        }
        data[offset..offset + written].copy_from_slice(&encoded[..written]);
        #[expect(clippy::cast_precision_loss, reason = "buffer lengths < 2^53")]
        Ok(jsg::Number::new(written as f64))
    }

    #[jsg_method]
    pub fn fill_impl(
        &self,
        mut buffer: jsg::v8::Local<jsg::v8::Uint8Array>,
        value: StringOrBuffer,
        start: jsg::Number,
        end: jsg::Number,
        encoding: Option<jsg::Number>,
    ) -> Result<(), jsg::Error> {
        let fill_bytes: Vec<u8> = match value {
            StringOrBuffer::Buffer(typed) => typed.as_slice().to_vec(),
            StringOrBuffer::String(s) => {
                let enc =
                    encoding.unwrap_or_else(|| jsg::Number::new(f64::from(Encoding::Utf8 as u8)));
                self.decode_string(s, enc)?
            }
        };

        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let start = (start.value().max(0.0) as usize).min(buffer.len());
        #[expect(clippy::cast_sign_loss, reason = "clamped to non-negative")]
        let end = (end.value().max(0.0) as usize).min(buffer.len());
        if start >= end {
            return Ok(());
        }
        let data = buffer.as_mut_slice();
        if fill_bytes.is_empty() {
            data[start..end].fill(0);
        } else {
            for i in start..end {
                data[i] = fill_bytes[(i - start) % fill_bytes.len()];
            }
        }
        Ok(())
    }

    #[jsg_method]
    pub fn index_of(
        &self,
        buffer: jsg::v8::Local<jsg::v8::Uint8Array>,
        value: StringOrBuffer,
        byte_offset: jsg::Number,
        encoding: jsg::Number,
        is_forward: jsg::NonCoercible<bool>,
    ) -> Result<jsg::Number, jsg::Error> {
        let is_forward = *is_forward;
        let needle: Vec<u8> = match value {
            StringOrBuffer::Buffer(typed) => typed.as_slice().to_vec(),
            StringOrBuffer::String(s) => self.decode_string(s, encoding)?,
        };

        let data = buffer.as_slice();
        // Use i64 for offset to avoid truncation for values outside i32 range.
        #[expect(
            clippy::cast_possible_truncation,
            reason = "clamped via i64 arithmetic below"
        )]
        let offset = byte_offset.value() as i64;
        let needle_length = i64::try_from(needle.len()).unwrap_or(i64::MAX);
        let len = i64::try_from(data.len()).unwrap_or(i64::MAX);

        // Compute starting offset (matches C++ indexOfOffset exactly).
        let opt_offset = if offset < 0 {
            if offset + len >= 0 {
                len + offset
            } else if is_forward || needle_length == 0 {
                0
            } else {
                -1
            }
        } else if offset + needle_length <= len {
            offset
        } else if needle_length == 0 {
            len
        } else if is_forward {
            -1
        } else {
            len - 1
        };

        // Empty needle returns the computed offset.
        if needle.is_empty() {
            #[expect(clippy::cast_precision_loss, reason = "buffer lengths < 2^53")]
            return Ok(jsg::Number::new(opt_offset as f64));
        }

        // No match possible.
        if data.is_empty()
            || opt_offset <= -1
            || (is_forward && {
                #[expect(clippy::cast_sign_loss, reason = "opt_offset verified non-negative")]
                let off = opt_offset as usize;
                needle.len() + off > data.len()
            })
            || needle.len() > data.len()
        {
            return Ok(jsg::Number::new(-1.0));
        }

        #[expect(clippy::cast_sign_loss, reason = "opt_offset verified non-negative")]
        let start = opt_offset as usize;
        if is_forward {
            let result = data[start..]
                .windows(needle.len())
                .position(|w| w == needle.as_slice())
                .map(|pos| start + pos);
            if let Some(pos) = result {
                #[expect(clippy::cast_precision_loss, reason = "buffer lengths < 2^53")]
                return Ok(jsg::Number::new(pos as f64));
            }
        } else {
            let search_end = start.min(data.len().saturating_sub(needle.len()));
            let result = data[..=search_end]
                .windows(needle.len())
                .rposition(|w| w == needle.as_slice());
            if let Some(pos) = result {
                #[expect(clippy::cast_precision_loss, reason = "buffer lengths < 2^53")]
                return Ok(jsg::Number::new(pos as f64));
            }
        }
        Ok(jsg::Number::new(-1.0))
    }

    #[jsg_method]
    pub fn transcode(
        &self,
        source: jsg::v8::Local<jsg::v8::Uint8Array>,
        from_encoding: jsg::Number,
        to_encoding: jsg::Number,
    ) -> Result<Vec<u8>, jsg::Error> {
        // V8 isolate limit — transcoding buffers larger than 128MB is not supported.
        const ISOLATE_MAX_SIZE: usize = 134_217_728;
        #[expect(clippy::cast_sign_loss, reason = "validated by Encoding::from_value")]
        let from_val = from_encoding.value().max(0.0) as u8;
        let from = Encoding::from_value(from_val)?;
        #[expect(clippy::cast_sign_loss, reason = "validated by Encoding::from_value")]
        let to_val = to_encoding.value().max(0.0) as u8;
        let to = Encoding::from_value(to_val)?;
        let data = source.as_slice();
        if from == to {
            return Ok(data.to_vec());
        }
        jsg_require!(
            data.len() <= ISOLATE_MAX_SIZE,
            Error,
            "Unable to transcode buffer"
        );
        // UTF-16LE source requires an even number of bytes.
        if from == Encoding::Utf16le {
            jsg_require!(
                data.len() % 2 == 0,
                RangeError,
                "Invalid UTF-16 input: odd byte count"
            );
        }
        // UTF-8 source must be valid UTF-8.
        if from == Encoding::Utf8 {
            jsg_require!(
                simdutf::validate_utf8(data),
                Error,
                "Unable to transcode buffer"
            );
        }
        match (from, to) {
            (Encoding::Latin1, Encoding::Utf8) => {
                let len = simdutf::utf8_length_from_latin1(data);
                let mut out = vec![0u8; len];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                unsafe {
                    let _ = simdutf::convert_latin1_to_utf8(
                        data.as_ptr(),
                        data.len(),
                        out.as_mut_ptr(),
                    );
                }
                Ok(out)
            }
            (Encoding::Utf8, Encoding::Latin1) => {
                // Convert UTF-8 → Latin1, replacing unmappable chars (> U+00FF) with '?'.
                // This matches ICU ucnv_convert behavior used by C++ transcode.
                let s = std::str::from_utf8(data)
                    .map_err(|_| jsg::Error::new_error("Unable to transcode buffer"))?;
                Ok(s.chars()
                    .map(|c| if (c as u32) <= 0xFF { c as u8 } else { b'?' })
                    .collect())
            }
            (Encoding::Utf8, Encoding::Utf16le) => {
                let u16_len = simdutf::utf16_length_from_utf8(data);
                let mut u16buf = vec![0u16; u16_len];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                unsafe {
                    let _ = simdutf::convert_utf8_to_utf16le(
                        data.as_ptr(),
                        data.len(),
                        u16buf.as_mut_ptr(),
                    );
                }
                Ok(u16buf
                    .iter()
                    .flat_map(|u| u.to_le_bytes())
                    .collect::<Vec<u8>>())
            }
            (Encoding::Utf16le, Encoding::Utf8) => {
                let u16s: Vec<u16> = data
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .collect();
                let len = simdutf::utf8_length_from_utf16le(&u16s);
                let mut out = vec![0u8; len];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                let written = unsafe {
                    simdutf::convert_utf16le_to_utf8(u16s.as_ptr(), u16s.len(), out.as_mut_ptr())
                };
                out.truncate(written);
                Ok(out)
            }
            (Encoding::Latin1, Encoding::Utf16le) => {
                let mut u16buf = vec![0u16; data.len()];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                unsafe {
                    let _ = simdutf::convert_latin1_to_utf16le(
                        data.as_ptr(),
                        data.len(),
                        u16buf.as_mut_ptr(),
                    );
                }
                Ok(u16buf
                    .iter()
                    .flat_map(|u| u.to_le_bytes())
                    .collect::<Vec<u8>>())
            }
            (Encoding::Utf16le, Encoding::Latin1) => {
                let u16s: Vec<u16> = data
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .collect();
                let mut out = vec![0u8; u16s.len()];
                // SAFETY: src/out are valid, non-overlapping, output buffer correctly sized.
                unsafe {
                    let _ = simdutf::convert_utf16le_to_latin1(
                        u16s.as_ptr(),
                        u16s.len(),
                        out.as_mut_ptr(),
                    );
                }
                Ok(out)
            }
            (Encoding::Ascii, t) => self.transcode(
                source,
                jsg::Number::new(1.0),
                jsg::Number::new(f64::from(t as u8)),
            ),
            (f, Encoding::Ascii) => {
                let mut r = self.transcode(
                    source,
                    jsg::Number::new(f64::from(f as u8)),
                    jsg::Number::new(1.0),
                )?;
                for b in &mut r {
                    *b &= 0x7f;
                }
                Ok(r)
            }
            _ => Err(jsg::Error::new_range_error(format!(
                "Unable to transcode from {from:?} to {to:?}"
            ))),
        }
    }

    #[jsg_method]
    pub fn decode(
        &self,
        input: &[u8],
        mut state: jsg::v8::Local<jsg::v8::Uint8Array>,
    ) -> Result<String, jsg::Error> {
        jsg_require!(state.len() == K_SIZE, TypeError, "Invalid StringDecoder");
        let encoding = Encoding::from_value(state.as_slice()[ENCODING_FIELD])?;

        // For ascii, latin1, and hex, there will never be left-over characters.
        if matches!(encoding, Encoding::Ascii | Encoding::Latin1 | Encoding::Hex) {
            return to_string_for_encoding(input, encoding);
        }

        // If bytes is empty there's nothing to decode.
        if input.is_empty() {
            return Ok(String::new());
        }

        let s = state.as_mut_slice();
        let data = input.to_vec();
        let mut data_offset: usize = 0;
        let mut nread = data.len();

        let mut prepend_bytes: Vec<u8> = Vec::new();

        let missing = s[MISSING_BYTES_FIELD] as usize;
        if missing > 0 {
            let buffered = s[BUFFERED_BYTES_FIELD] as usize;
            jsg_require!(
                missing + buffered <= 4,
                Error,
                "Invalid StringDecoder state"
            );

            if encoding == Encoding::Utf8 {
                // For UTF-8, we need special treatment to align with the V8 decoder:
                // If an incomplete character is found at a chunk boundary, we use
                // its remainder and pass it to V8 as-is.
                let check_limit = nread.min(missing);
                for i in 0..check_limit {
                    if (data[i] & 0xC0) != 0x80 {
                        // This byte is not a continuation byte even though it should have
                        // been one. We stop decoding of the incomplete character at this
                        // point (but still use the rest of the incomplete bytes from this
                        // chunk) and assume that the new, unexpected byte starts a new one.
                        s[MISSING_BYTES_FIELD] = 0;
                        let cur_buffered = s[BUFFERED_BYTES_FIELD] as usize;
                        s[INCOMPLETE_START + cur_buffered..INCOMPLETE_START + cur_buffered + i]
                            .copy_from_slice(&data[..i]);
                        s[BUFFERED_BYTES_FIELD] += i as u8;
                        data_offset += i;
                        nread -= i;
                        break;
                    }
                }
            }

            // Re-read missing after potential modification above.
            let current_missing = s[MISSING_BYTES_FIELD] as usize;
            let current_buffered = s[BUFFERED_BYTES_FIELD] as usize;
            let found_bytes = nread.min(current_missing);
            s[INCOMPLETE_START + current_buffered
                ..INCOMPLETE_START + current_buffered + found_bytes]
                .copy_from_slice(&data[data_offset..data_offset + found_bytes]);
            data_offset += found_bytes;
            nread -= found_bytes;

            s[MISSING_BYTES_FIELD] -= found_bytes as u8;
            s[BUFFERED_BYTES_FIELD] += found_bytes as u8;

            if s[MISSING_BYTES_FIELD] == 0 {
                // Character is complete — keep as raw bytes so we can combine
                // with the body bytes before string conversion. This is critical
                // for UTF-16LE where surrogate pairs may be split across the
                // prepend/body boundary.
                let buf_bytes = s[BUFFERED_BYTES_FIELD] as usize;
                prepend_bytes = s[INCOMPLETE_START..INCOMPLETE_START + buf_bytes].to_vec();
                s[BUFFERED_BYTES_FIELD] = 0;
            }
        }

        if nread == 0 {
            return to_string_for_encoding(&prepend_bytes, encoding);
        }
        jsg_require!(
            s[MISSING_BYTES_FIELD] == 0 && s[BUFFERED_BYTES_FIELD] == 0,
            Error,
            "Invalid StringDecoder state"
        );

        let data_slice = &data[data_offset..data_offset + nread];

        // See whether there is a character that we may have to cut off and
        // finish when receiving the next chunk.
        if encoding == Encoding::Utf8 && (data_slice[nread - 1] & 0x80) != 0 {
            // This is UTF-8 encoded data and we ended on a non-ASCII UTF-8 byte.
            // This means we'll need to figure out where the character to which
            // the byte belongs begins.
            let mut i = nread - 1;
            loop {
                jsg_require!(i < nread, Error, "Invalid StringDecoder state");
                s[BUFFERED_BYTES_FIELD] += 1;
                if (data_slice[i] & 0xC0) == 0x80 {
                    // This byte does not start a character (a "trailing" byte).
                    if s[BUFFERED_BYTES_FIELD] >= 4 || i == 0 {
                        // We either have more than 4 trailing bytes (which means
                        // the current character would not be inside the range for
                        // valid Unicode), or the current buffer does not contain
                        // the start of a UTF-8 character at all. Either way, this
                        // is invalid UTF8 and we can just let the engine's decoder
                        // handle it.
                        s[BUFFERED_BYTES_FIELD] = 0;
                        break;
                    }
                } else {
                    // Found the first byte of a UTF-8 character. By looking at the
                    // upper bits we can tell how long the character *should* be.
                    if (data_slice[i] & 0xE0) == 0xC0 {
                        s[MISSING_BYTES_FIELD] = 2;
                    } else if (data_slice[i] & 0xF0) == 0xE0 {
                        s[MISSING_BYTES_FIELD] = 3;
                    } else if (data_slice[i] & 0xF8) == 0xF0 {
                        s[MISSING_BYTES_FIELD] = 4;
                    } else {
                        // This lead byte would indicate a character outside of the
                        // representable range.
                        s[BUFFERED_BYTES_FIELD] = 0;
                        break;
                    }

                    if s[BUFFERED_BYTES_FIELD] >= s[MISSING_BYTES_FIELD] {
                        // Received more or exactly as many trailing bytes than the lead
                        // character would indicate. In the "==" case, we have valid
                        // data and don't need to slice anything off;
                        // in the ">" case, this is invalid UTF-8 anyway.
                        s[MISSING_BYTES_FIELD] = 0;
                        s[BUFFERED_BYTES_FIELD] = 0;
                    }

                    s[MISSING_BYTES_FIELD] -= s[BUFFERED_BYTES_FIELD];
                    break;
                }
                if i == 0 {
                    break;
                }
                i -= 1;
            }
        } else if encoding == Encoding::Utf16le {
            if (nread % 2) == 1 {
                if nread >= 3 && (data_slice[nread - 2] & 0xFC) == 0xD8 {
                    // Odd byte at end, preceded by a high surrogate.
                    // Buffer all 3 bytes (high surrogate + trailing byte)
                    // so the low surrogate can be completed next chunk.
                    s[BUFFERED_BYTES_FIELD] = 3;
                } else {
                    // Got half a codepoint, and need the second byte of it.
                    s[BUFFERED_BYTES_FIELD] = 1;
                }
                s[MISSING_BYTES_FIELD] = 1;
            } else if (data_slice[nread - 1] & 0xFC) == 0xD8 {
                // Half a split UTF-16 character.
                s[BUFFERED_BYTES_FIELD] = 2;
                s[MISSING_BYTES_FIELD] = 2;
            }
        } else if encoding == Encoding::Base64 || encoding == Encoding::Base64url {
            s[BUFFERED_BYTES_FIELD] = (nread % 3) as u8;
            if s[BUFFERED_BYTES_FIELD] > 0 {
                s[MISSING_BYTES_FIELD] = 3 - s[BUFFERED_BYTES_FIELD];
            }
        }

        let buf_bytes = s[BUFFERED_BYTES_FIELD] as usize;
        if buf_bytes > 0 {
            // Copy the requested number of buffered bytes from the end of the
            // input into the incomplete character buffer.
            nread -= buf_bytes;
            s[INCOMPLETE_START..INCOMPLETE_START + buf_bytes]
                .copy_from_slice(&data_slice[nread..nread + buf_bytes]);
        }

        // Combine prepend and body raw bytes before string conversion.
        // This is critical for UTF-16LE where surrogate pairs may be split
        // across the prepend/body boundary — converting separately would
        // lose unpaired surrogates during UTF-16→UTF-8 conversion.
        let bytes = if prepend_bytes.is_empty() {
            data_slice[..nread].to_vec()
        } else if nread > 0 {
            let mut combined = Vec::with_capacity(prepend_bytes.len() + nread);
            combined.extend_from_slice(&prepend_bytes);
            combined.extend_from_slice(&data_slice[..nread]);
            combined
        } else {
            prepend_bytes
        };
        to_string_for_encoding(&bytes, encoding)
    }

    #[jsg_method]
    pub fn flush(
        &self,
        mut state: jsg::v8::Local<jsg::v8::Uint8Array>,
    ) -> Result<String, jsg::Error> {
        jsg_require!(state.len() == K_SIZE, TypeError, "Invalid StringDecoder");
        let s = state.as_mut_slice();
        let encoding = Encoding::from_value(s[ENCODING_FIELD])?;

        if matches!(encoding, Encoding::Ascii | Encoding::Hex | Encoding::Latin1) {
            jsg_require!(
                s[MISSING_BYTES_FIELD] == 0,
                Error,
                "Invalid StringDecoder state"
            );
            jsg_require!(
                s[BUFFERED_BYTES_FIELD] == 0,
                Error,
                "Invalid StringDecoder state"
            );
        }

        if encoding == Encoding::Utf16le && s[BUFFERED_BYTES_FIELD] % 2 == 1 {
            // Ignore a single trailing byte, like the JS decoder does.
            s[MISSING_BYTES_FIELD] -= 1;
            s[BUFFERED_BYTES_FIELD] -= 1;
        }

        let buffered = s[BUFFERED_BYTES_FIELD] as usize;
        if buffered == 0 {
            return Ok(String::new());
        }

        let incomplete = s[INCOMPLETE_START..INCOMPLETE_START + buffered].to_vec();
        let result = to_string_for_encoding(&incomplete, encoding)?;
        s[BUFFERED_BYTES_FIELD] = 0;
        s[MISSING_BYTES_FIELD] = 0;
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Construct a BufferUtil for calling instance methods in tests.
    fn buf() -> BufferUtil {
        BufferUtil
    }

    // =========================================================================
    // Encoding
    // =========================================================================

    #[test]
    fn encoding_from_value_all_valid() {
        assert_eq!(Encoding::from_value(0).unwrap(), Encoding::Ascii);
        assert_eq!(Encoding::from_value(1).unwrap(), Encoding::Latin1);
        assert_eq!(Encoding::from_value(2).unwrap(), Encoding::Utf8);
        assert_eq!(Encoding::from_value(3).unwrap(), Encoding::Utf16le);
        assert_eq!(Encoding::from_value(4).unwrap(), Encoding::Base64);
        assert_eq!(Encoding::from_value(5).unwrap(), Encoding::Base64url);
        assert_eq!(Encoding::from_value(6).unwrap(), Encoding::Hex);
    }

    #[test]
    fn encoding_from_value_out_of_range() {
        assert!(Encoding::from_value(7).is_err());
        assert!(Encoding::from_value(255).is_err());
    }

    // =========================================================================
    // BufferUtil::byte_length
    // =========================================================================

    #[test]
    fn byte_length_empty() {
        assert!((buf().byte_length(String::new()).value() - 0.0).abs() < f64::EPSILON);
    }

    #[test]
    fn byte_length_ascii() {
        assert!((buf().byte_length("Hello".to_owned()).value() - 5.0).abs() < f64::EPSILON);
    }

    #[test]
    fn byte_length_multibyte() {
        // "café" is 5 UTF-8 bytes (c=1, a=1, f=1, é=2)
        assert!((buf().byte_length("café".to_owned()).value() - 5.0).abs() < f64::EPSILON);
    }

    #[test]
    fn byte_length_emoji() {
        // 🎉 is 4 UTF-8 bytes
        assert!((buf().byte_length("🎉".to_owned()).value() - 4.0).abs() < f64::EPSILON);
    }

    // =========================================================================
    // BufferUtil::concat
    // =========================================================================

    #[test]
    fn concat_basic() {
        let result = buf()
            .concat(vec![vec![1, 2], vec![3, 4]], jsg::Number::new(4.0))
            .unwrap();
        assert_eq!(result, vec![1, 2, 3, 4]);
    }

    #[test]
    fn concat_truncates() {
        let result = buf()
            .concat(vec![vec![1, 2, 3], vec![4, 5, 6]], jsg::Number::new(4.0))
            .unwrap();
        assert_eq!(result, vec![1, 2, 3, 4]);
    }

    #[test]
    fn concat_zero_pads() {
        let result = buf()
            .concat(vec![vec![1, 2]], jsg::Number::new(5.0))
            .unwrap();
        assert_eq!(result, vec![1, 2, 0, 0, 0]);
    }

    #[test]
    fn concat_empty_list() {
        let result = buf().concat(vec![], jsg::Number::new(3.0)).unwrap();
        assert_eq!(result, vec![0, 0, 0]);
    }

    #[test]
    fn concat_zero_length() {
        let result = buf()
            .concat(vec![vec![1, 2, 3]], jsg::Number::new(0.0))
            .unwrap();
        assert_eq!(result, vec![]);
    }

    #[test]
    fn concat_length_too_large() {
        let result = buf().concat(vec![], jsg::Number::new(3_000_000_000.0));
        assert!(result.is_err());
    }

    // =========================================================================
    // BufferUtil::decode_string (encoding logic)
    // =========================================================================

    #[test]
    fn decode_string_ascii() {
        let result = buf()
            .decode_string(
                "Hello".to_owned(),
                jsg::Number::new(f64::from(Encoding::Ascii as u8)),
            )
            .unwrap();
        assert_eq!(result, b"Hello");
    }

    #[test]
    fn decode_string_utf8() {
        let result = buf()
            .decode_string(
                "café".to_owned(),
                jsg::Number::new(f64::from(Encoding::Utf8 as u8)),
            )
            .unwrap();
        assert_eq!(result, "café".as_bytes());
    }

    #[test]
    fn decode_string_hex() {
        let result = buf()
            .decode_string(
                "48656c6c6f".to_owned(),
                jsg::Number::new(f64::from(Encoding::Hex as u8)),
            )
            .unwrap();
        assert_eq!(result, b"Hello");
    }

    #[test]
    fn decode_string_hex_invalid_truncates() {
        // Non-strict hex decode truncates at first invalid pair (Node.js behavior)
        let result = buf()
            .decode_string(
                "zzzz".to_owned(),
                jsg::Number::new(f64::from(Encoding::Hex as u8)),
            )
            .unwrap();
        assert_eq!(result, vec![]);
    }

    #[test]
    fn decode_string_hex_partial_valid() {
        let result = buf()
            .decode_string(
                "aazz".to_owned(),
                jsg::Number::new(f64::from(Encoding::Hex as u8)),
            )
            .unwrap();
        assert_eq!(result, vec![0xaa]);
    }

    #[test]
    fn decode_string_base64() {
        let result = buf()
            .decode_string(
                "SGVsbG8=".to_owned(),
                jsg::Number::new(f64::from(Encoding::Base64 as u8)),
            )
            .unwrap();
        assert_eq!(result, b"Hello");
    }

    #[test]
    fn decode_string_base64url() {
        let result = buf()
            .decode_string(
                "SGVsbG8".to_owned(),
                jsg::Number::new(f64::from(Encoding::Base64url as u8)),
            )
            .unwrap();
        assert_eq!(result, b"Hello");
    }

    #[test]
    fn decode_string_invalid_encoding() {
        let result = buf().decode_string("x".to_owned(), jsg::Number::new(99.0));
        assert!(result.is_err());
    }

    // =========================================================================
    // Hex encoding/decoding (internal helpers used by BufferUtil methods)
    // =========================================================================

    #[test]
    fn hex_encode_empty() {
        assert_eq!(hex_encode(&[]), "");
    }

    #[test]
    fn hex_encode_basic() {
        assert_eq!(hex_encode(&[0x48, 0x65, 0x6c, 0x6c, 0x6f]), "48656c6c6f");
    }

    #[test]
    fn hex_encode_all_byte_values() {
        assert_eq!(hex_encode(&[0x00, 0xff, 0x0a, 0xf0]), "00ff0af0");
    }

    #[test]
    fn hex_decode_empty() {
        assert_eq!(hex_decode(b"", false).unwrap(), vec![]);
    }

    #[test]
    fn hex_decode_basic() {
        assert_eq!(hex_decode(b"48656c6c6f", false).unwrap(), b"Hello");
    }

    #[test]
    fn hex_decode_uppercase() {
        assert_eq!(hex_decode(b"AABB", false).unwrap(), vec![0xaa, 0xbb]);
    }

    #[test]
    fn hex_decode_mixed_case() {
        assert_eq!(hex_decode(b"aAbB", false).unwrap(), vec![0xaa, 0xbb]);
    }

    #[test]
    fn hex_decode_odd_length_truncates() {
        // Node.js behavior: trailing nibble is ignored
        assert_eq!(hex_decode(b"aab", false).unwrap(), vec![0xaa]);
    }

    #[test]
    fn hex_decode_invalid_char_truncates() {
        // Non-strict mode: truncates at first invalid pair
        assert_eq!(hex_decode(b"zz", false).unwrap(), vec![]);
        assert_eq!(hex_decode(b"aazz", false).unwrap(), vec![0xaa]);
    }

    #[test]
    fn hex_decode_strict_rejects() {
        assert!(hex_decode(b"zz", true).is_err());
        assert!(hex_decode(b"aab", true).is_err());
    }

    #[test]
    fn hex_roundtrip() {
        let data: Vec<u8> = (0..=255).collect();
        let encoded = hex_encode(&data);
        let decoded = hex_decode(encoded.as_bytes(), false).unwrap();
        assert_eq!(decoded, data);
    }

    #[test]
    fn hex_val_digits() {
        for (i, c) in b"0123456789".iter().enumerate() {
            assert_eq!(hex_val(*c), Some(i as u8));
        }
    }

    #[test]
    fn hex_val_lowercase() {
        for (i, c) in b"abcdef".iter().enumerate() {
            assert_eq!(hex_val(*c), Some(10 + i as u8));
        }
    }

    #[test]
    fn hex_val_uppercase() {
        for (i, c) in b"ABCDEF".iter().enumerate() {
            assert_eq!(hex_val(*c), Some(10 + i as u8));
        }
    }

    #[test]
    fn hex_val_invalid() {
        assert_eq!(hex_val(b'g'), None);
        assert_eq!(hex_val(b'z'), None);
        assert_eq!(hex_val(b' '), None);
        assert_eq!(hex_val(0), None);
    }

    // =========================================================================
    // BufferUtil::constructor / constants
    // =========================================================================

    #[test]
    fn encoding_constants_match_enum() {
        assert_eq!(BufferUtil::ASCII, Encoding::Ascii as u8);
        assert_eq!(BufferUtil::LATIN1, Encoding::Latin1 as u8);
        assert_eq!(BufferUtil::UTF8, Encoding::Utf8 as u8);
        assert_eq!(BufferUtil::UTF16LE, Encoding::Utf16le as u8);
        assert_eq!(BufferUtil::BASE64, Encoding::Base64 as u8);
        assert_eq!(BufferUtil::BASE64URL, Encoding::Base64url as u8);
        assert_eq!(BufferUtil::HEX, Encoding::Hex as u8);
    }

    // =========================================================================
    // BufferUtil::write (logic via decode_string)
    // =========================================================================

    #[test]
    fn write_decode_roundtrip_utf8() {
        let encoded = buf()
            .decode_string(
                "Hello".to_owned(),
                jsg::Number::new(f64::from(Encoding::Utf8 as u8)),
            )
            .unwrap();
        assert_eq!(encoded, b"Hello");
    }

    #[test]
    fn write_decode_roundtrip_latin1() {
        let encoded = buf()
            .decode_string(
                "A".to_owned(),
                jsg::Number::new(f64::from(Encoding::Latin1 as u8)),
            )
            .unwrap();
        // Latin1 encoding of ASCII 'A' is just 0x41
        assert_eq!(encoded, vec![0x41]);
    }

    #[test]
    fn write_decode_roundtrip_utf16le() {
        let encoded = buf()
            .decode_string(
                "AB".to_owned(),
                jsg::Number::new(f64::from(Encoding::Utf16le as u8)),
            )
            .unwrap();
        // UTF-16LE: A=0x41,0x00  B=0x42,0x00
        assert_eq!(encoded, vec![0x41, 0x00, 0x42, 0x00]);
    }

    #[test]
    fn write_decode_base64_roundtrip() {
        let encoded = buf()
            .decode_string(
                "SGVsbG8=".to_owned(),
                jsg::Number::new(f64::from(Encoding::Base64 as u8)),
            )
            .unwrap();
        assert_eq!(encoded, b"Hello");
    }
}
