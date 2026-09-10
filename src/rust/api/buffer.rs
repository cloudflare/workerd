// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust implementation of `node-internal:buffer`.
//!
//! This follows `src/workerd/api/node/buffer.c++` deliberately closely. The
//! C++ implementation remains available behind an autogate as the behavioral
//! oracle and rollback path.

use std::cmp::Ordering;

use jsg::FromJS;
use jsg::ToJS;
use jsg::Type;
use jsg::jsg_fail_require;
use jsg::jsg_require;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Encoding {
    Ascii = 0,
    Latin1 = 1,
    Utf8 = 2,
    Utf16Le = 3,
    Base64 = 4,
    Base64Url = 5,
    Hex = 6,
}

impl Encoding {
    fn from_value(value: u8) -> jsg::Result<Self> {
        match value {
            0 => Ok(Self::Ascii),
            1 => Ok(Self::Latin1),
            2 => Ok(Self::Utf8),
            3 => Ok(Self::Utf16Le),
            4 => Ok(Self::Base64),
            5 => Ok(Self::Base64Url),
            6 => Ok(Self::Hex),
            _ => Err(jsg::Error::new_error("Invalid encoding")),
        }
    }

    fn can_be_transcoded(self) -> bool {
        matches!(
            self,
            Self::Ascii | Self::Latin1 | Self::Utf8 | Self::Utf16Le
        )
    }
}

pub struct CompareOptions {
    pub a_start: Option<jsg::Number>,
    pub a_end: Option<jsg::Number>,
    pub b_start: Option<jsg::Number>,
    pub b_end: Option<jsg::Number>,
}

impl Type for CompareOptions {
    fn class_name() -> &'static str {
        "CompareOptions"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_object()
    }
}

impl FromJS for CompareOptions {
    type ResultType = Self;

    fn from_js(lock: &mut jsg::Lock, value: v8::Local<v8::Value>) -> jsg::Result<Self> {
        if !value.is_object() {
            return Err(jsg::Error::new_type_error("expected CompareOptions object"));
        }
        let object: v8::Local<v8::Object> = value.into();
        let number = |name: &str, lock: &mut jsg::Lock| -> jsg::Result<Option<jsg::Number>> {
            object
                .get(lock, name)
                .map(|value| jsg::Number::from_js(lock, value))
                .transpose()
        };
        Ok(Self {
            a_start: number("aStart", lock)?,
            a_end: number("aEnd", lock)?,
            b_start: number("bStart", lock)?,
            b_end: number("bEnd", lock)?,
        })
    }
}

enum StringOrBuffer<'a> {
    String(v8::Local<'a, v8::String>),
    Buffer(v8::Local<'a, v8::Uint8Array>),
}

impl Type for StringOrBuffer<'_> {
    fn class_name() -> &'static str {
        "string or Uint8Array"
    }

    fn is_exact(value: &v8::Local<v8::Value>) -> bool {
        value.is_string() || value.is_uint8_array()
    }
}

impl FromJS for StringOrBuffer<'_> {
    type ResultType = Self;

    fn from_js(lock: &mut jsg::Lock, value: v8::Local<v8::Value>) -> jsg::Result<Self> {
        if value.is_string() {
            return v8::Local::<v8::String>::from_js(lock, value).map(Self::String);
        }
        if value.is_uint8_array() {
            return v8::Local::<v8::Uint8Array>::from_js(lock, value).map(Self::Buffer);
        }
        Err(jsg::Error::new_type_error("expected string or Uint8Array"))
    }
}

const INCOMPLETE_START: usize = 0;
const INCOMPLETE_END: usize = 4;
const MISSING_BYTES: usize = 4;
const BUFFERED_BYTES: usize = 5;
const ENCODING: usize = 6;
const STATE_SIZE: usize = 7;

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn decode_hex_truncated(text: &[u8], strict: bool) -> jsg::Result<Vec<u8>> {
    if !text.len().is_multiple_of(2) && strict {
        return Err(jsg::Error::new_type_error("The text is not valid hex"));
    }

    let mut output = Vec::with_capacity(text.len() / 2);
    for pair in text[..text.len() & !1].chunks_exact(2) {
        let Some(high) = hex_value(pair[0]) else {
            if strict {
                return Err(jsg::Error::new_type_error("The text is not valid hex"));
            }
            break;
        };
        let Some(low) = hex_value(pair[1]) else {
            if strict {
                return Err(jsg::Error::new_type_error("The text is not valid hex"));
            }
            break;
        };
        output.push((high << 4) | low);
    }
    Ok(output)
}

fn decode_hex_into(output: &mut [u8], text: &[u8]) -> usize {
    let mut written = 0;
    for pair in text.chunks_exact(2).take(output.len()) {
        let (Some(high), Some(low)) = (hex_value(pair[0]), hex_value(pair[1])) else {
            break;
        };
        output[written] = (high << 4) | low;
        written += 1;
    }
    written
}

fn encode_hex(input: &[u8]) -> Vec<u8> {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut output = Vec::with_capacity(input.len().saturating_mul(2));
    for byte in input {
        output.push(DIGITS[usize::from(byte >> 4)]);
        output.push(DIGITS[usize::from(byte & 0x0f)]);
    }
    output
}

fn require_v8_string_length(length: usize) -> jsg::Result<()> {
    jsg_require!(
        length <= v8::String::MAX_LENGTH as usize,
        RangeError,
        "String is too long for a V8 string"
    );
    Ok(())
}

fn string_to_bytes(
    lock: &mut jsg::Lock,
    string: &v8::Local<v8::String>,
    encoding: Encoding,
    strict_hex: bool,
) -> jsg::Result<Vec<u8>> {
    let length = usize::try_from(string.length()).unwrap_or_default();
    if length == 0 {
        return Ok(Vec::new());
    }

    match encoding {
        Encoding::Ascii | Encoding::Latin1 => {
            let mut output = vec![0; length];
            string.write_one_byte(
                lock,
                0,
                length as u32,
                &mut output,
                v8::WriteFlags::ReplaceInvalidUtf8,
            );
            Ok(output)
        }
        Encoding::Utf8 => {
            let mut output = vec![0; string.utf8_length(lock)];
            let written = string.write_utf8(lock, &mut output, v8::WriteFlags::ReplaceInvalidUtf8);
            output.truncate(written);
            Ok(output)
        }
        Encoding::Utf16Le => {
            let mut units = vec![0; length];
            string.write(
                lock,
                0,
                length as u32,
                &mut units,
                v8::WriteFlags::ReplaceInvalidUtf8,
            );
            Ok(units.into_iter().flat_map(u16::to_le_bytes).collect())
        }
        Encoding::Base64 | Encoding::Base64Url => {
            let mut text = vec![0; length];
            string.write_one_byte(
                lock,
                0,
                length as u32,
                &mut text,
                v8::WriteFlags::ReplaceInvalidUtf8,
            );
            let mut output = vec![0; nbytes::base64_decoded_size(&text)];
            let written = nbytes::base64_decode_into(&mut output, &text);
            output.truncate(written);
            Ok(output)
        }
        Encoding::Hex => {
            let mut text = vec![0; length];
            string.write_one_byte(
                lock,
                0,
                length as u32,
                &mut text,
                v8::WriteFlags::ReplaceInvalidUtf8,
            );
            decode_hex_truncated(&text, strict_hex)
        }
    }
}

fn string_from_bytes<'a>(
    lock: &mut jsg::Lock,
    input: &[u8],
    encoding: Encoding,
) -> jsg::Result<v8::Local<'a, v8::String>> {
    let maybe = match encoding {
        Encoding::Ascii => {
            require_v8_string_length(input.len())?;
            let masked: Vec<u8> = input.iter().map(|byte| byte & 0x7f).collect();
            return v8::String::new_from_one_byte(lock, &masked)
                .into_option(lock)
                .ok_or_else(|| jsg::Error::new_error("Failed to create string"));
        }
        Encoding::Latin1 => {
            require_v8_string_length(input.len())?;
            v8::String::new_from_one_byte(lock, input)
        }
        Encoding::Utf8 => {
            require_v8_string_length(input.len())?;
            v8::String::new_from_utf8(lock, input)
        }
        Encoding::Utf16Le => {
            require_v8_string_length(input.len() / 2)?;
            let units: Vec<u16> = input
                .chunks_exact(2)
                .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
                .collect();
            return v8::String::new_from_two_byte(lock, &units)
                .into_option(lock)
                .ok_or_else(|| jsg::Error::new_error("Failed to create string"));
        }
        Encoding::Base64 => {
            let encoded_length = nbytes::base64_encoded_size(input.len(), false);
            require_v8_string_length(encoded_length)?;
            let encoded = nbytes::base64_encode(input, false);
            return v8::String::new_from_one_byte(lock, &encoded)
                .into_option(lock)
                .ok_or_else(|| jsg::Error::new_error("Failed to create string"));
        }
        Encoding::Base64Url => {
            let encoded_length = nbytes::base64_encoded_size(input.len(), true);
            require_v8_string_length(encoded_length)?;
            let encoded = nbytes::base64_encode(input, true);
            return v8::String::new_from_one_byte(lock, &encoded)
                .into_option(lock)
                .ok_or_else(|| jsg::Error::new_error("Failed to create string"));
        }
        Encoding::Hex => {
            require_v8_string_length(input.len().saturating_mul(2))?;
            let encoded = encode_hex(input);
            return v8::String::new_from_one_byte(lock, &encoded)
                .into_option(lock)
                .ok_or_else(|| jsg::Error::new_error("Failed to create string"));
        }
    };
    maybe
        .into_option(lock)
        .ok_or_else(|| jsg::Error::new_error("Failed to create string"))
}

fn new_uint8_array<'a>(
    lock: &mut jsg::Lock,
    length: usize,
) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
    let buffer = v8::ArrayBuffer::new_with_mode(
        lock,
        length,
        v8::ffi::BackingStoreInitializationMode::ZeroInitialized,
    )
    .ok_or_else(|| jsg::Error::new_range_error("Failed to allocate memory for Uint8Array"))?;
    Ok(v8::Uint8Array::from_buffer(lock, &buffer, 0, length))
}

fn index_of_offset(length: usize, offset: i32, needle_length: usize, forward: bool) -> i32 {
    let len = i32::try_from(length).unwrap_or(i32::MAX);
    let needle = i32::try_from(needle_length).unwrap_or(i32::MAX);
    if offset < 0 {
        if offset.saturating_add(len) >= 0 {
            len + offset
        } else if forward || needle == 0 {
            0
        } else {
            -1
        }
    } else if i64::from(offset) + i64::from(needle) <= i64::from(len) {
        offset
    } else if needle == 0 {
        len
    } else if forward {
        -1
    } else {
        len - 1
    }
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "JSG applies JavaScript's numeric conversion before these bounded uses"
)]
fn number_to_usize(value: jsg::Number) -> usize {
    value.value() as usize
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "encoding values are small integer constants supplied by internal TypeScript"
)]
fn number_to_u8(value: jsg::Number) -> u8 {
    value.value() as u8
}

fn find_bytes(haystack: &[u8], needle: &[u8], offset: usize, forward: bool) -> Option<usize> {
    if forward {
        haystack
            .get(offset..)?
            .windows(needle.len())
            .position(|window| window == needle)
            .map(|index| offset + index)
    } else {
        let last = offset.min(haystack.len().saturating_sub(needle.len()));
        haystack
            .get(..last.saturating_add(needle.len()))?
            .windows(needle.len())
            .rposition(|window| window == needle)
    }
}

fn find_utf16(haystack: &[u8], needle: &[u8], offset: usize, forward: bool) -> Option<usize> {
    let haystack: Vec<u16> = haystack
        .chunks_exact(2)
        .map(|pair| u16::from_ne_bytes([pair[0], pair[1]]))
        .collect();
    let needle: Vec<u16> = needle
        .chunks_exact(2)
        .map(|pair| u16::from_ne_bytes([pair[0], pair[1]]))
        .collect();
    find_words(&haystack, &needle, offset / 2, forward).map(|index| index * 2)
}

fn find_words(haystack: &[u16], needle: &[u16], offset: usize, forward: bool) -> Option<usize> {
    if forward {
        haystack
            .get(offset..)?
            .windows(needle.len())
            .position(|window| window == needle)
            .map(|index| offset + index)
    } else {
        let last = offset.min(haystack.len().saturating_sub(needle.len()));
        haystack
            .get(..last.saturating_add(needle.len()))?
            .windows(needle.len())
            .rposition(|window| window == needle)
    }
}

#[jsg_resource]
pub struct BufferUtil;

#[jsg_resource]
impl BufferUtil {
    #[must_use]
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self)
    }

    #[jsg_static_constant]
    pub const ASCII: u8 = Encoding::Ascii as u8;
    #[jsg_static_constant]
    pub const LATIN1: u8 = Encoding::Latin1 as u8;
    #[jsg_static_constant]
    pub const UTF8: u8 = Encoding::Utf8 as u8;
    #[jsg_static_constant]
    pub const UTF16LE: u8 = Encoding::Utf16Le as u8;
    #[jsg_static_constant]
    pub const BASE64: u8 = Encoding::Base64 as u8;
    #[jsg_static_constant]
    pub const BASE64URL: u8 = Encoding::Base64Url as u8;
    #[jsg_static_constant]
    pub const HEX: u8 = Encoding::Hex as u8;

    #[jsg_method]
    pub fn byte_length(&self, lock: &mut jsg::Lock, string: v8::Local<v8::String>) -> jsg::Number {
        let length = u32::try_from(string.utf8_length(lock)).unwrap_or(u32::MAX);
        jsg::Number::new(f64::from(length))
    }

    #[jsg_method]
    pub fn compare(
        &self,
        one: v8::Local<v8::Uint8Array>,
        two: v8::Local<v8::Uint8Array>,
        options: Option<CompareOptions>,
    ) -> jsg::Number {
        let mut one = one.as_slice();
        let mut two = two.as_slice();
        if let Some(options) = options {
            let end = options
                .a_end
                .map_or(one.len(), number_to_usize)
                .min(one.len());
            let start = options.a_start.map_or(0, number_to_usize).min(end);
            one = &one[start..end];
            let end = options
                .b_end
                .map_or(two.len(), number_to_usize)
                .min(two.len());
            let start = options.b_start.map_or(0, number_to_usize).min(end);
            two = &two[start..end];
        }
        jsg::Number::new(f64::from(match one.cmp(two) {
            Ordering::Less => -1,
            Ordering::Equal => 0,
            Ordering::Greater => 1,
        }))
    }

    #[jsg_method]
    pub fn concat<'a>(
        &self,
        lock: &mut jsg::Lock,
        list: Vec<v8::Local<v8::Uint8Array>>,
        length: u32,
    ) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
        let length = length as usize;
        jsg_require!(
            i32::try_from(length).is_ok(),
            RangeError,
            "The length is too large"
        );
        let mut output = new_uint8_array(lock, length)?;
        let mut offset = 0;
        // SAFETY: output was allocated above and has not been exposed or aliased.
        let destination = unsafe { output.as_mut_slice(lock) };
        for source in list {
            let count = source.len().min(destination.len().saturating_sub(offset));
            if count == 0 {
                if offset == destination.len() {
                    break;
                }
                continue;
            }
            destination[offset..offset + count].copy_from_slice(&source.as_slice()[..count]);
            offset += count;
        }
        Ok(output)
    }

    #[jsg_method]
    pub fn decode_string<'a>(
        &self,
        lock: &mut jsg::Lock,
        string: v8::Local<v8::String>,
        encoding: u8,
    ) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
        let bytes = string_to_bytes(lock, &string, Encoding::from_value(encoding)?, false)?;
        Ok(bytes.to_js(lock).into())
    }

    #[jsg_method]
    pub fn fill_impl(
        &self,
        lock: &mut jsg::Lock,
        mut buffer: v8::Local<v8::Uint8Array>,
        value: StringOrBuffer,
        start: u32,
        end: u32,
        encoding: Option<jsg::Number>,
    ) -> jsg::Result<()> {
        let end = (end as usize).min(buffer.len());
        let start = start as usize;
        if end <= start {
            return Ok(());
        }
        let fill = match value {
            StringOrBuffer::String(string) => string_to_bytes(
                lock,
                &string,
                Encoding::from_value(encoding.map_or(Self::UTF8, number_to_u8))?,
                true,
            )?,
            StringOrBuffer::Buffer(source) => source.as_slice().to_vec(),
        };
        // SAFETY: the source was copied above, so no Rust reference aliases
        // this destination view during the mutation.
        let destination = unsafe { buffer.as_mut_slice(lock) };
        if fill.is_empty() {
            destination[start..end].fill(0);
        } else {
            for (index, byte) in destination[start..end].iter_mut().enumerate() {
                *byte = fill[index % fill.len()];
            }
        }
        Ok(())
    }

    #[jsg_method]
    pub fn index_of(
        &self,
        lock: &mut jsg::Lock,
        buffer: v8::Local<v8::Uint8Array>,
        value: StringOrBuffer,
        byte_offset: i32,
        encoding: u8,
        forward: bool,
    ) -> jsg::Result<Option<jsg::Number>> {
        let encoding = Encoding::from_value(encoding)?;
        let needle = match value {
            StringOrBuffer::String(string) => string_to_bytes(lock, &string, encoding, false)?,
            StringOrBuffer::Buffer(source) => source.as_slice().to_vec(),
        };
        let bytes = buffer.as_slice();
        let length = if encoding == Encoding::Utf16Le {
            bytes.len() & !1
        } else {
            bytes.len()
        };
        let offset = index_of_offset(length, byte_offset, needle.len(), forward);
        if needle.is_empty() {
            return Ok(u32::try_from(offset)
                .ok()
                .map(|value| jsg::Number::new(f64::from(value))));
        }
        if length == 0 || offset < 0 || needle.len() > length {
            return Ok(None);
        }
        let offset = usize::try_from(offset).unwrap_or_default();
        if forward && needle.len() + offset > length {
            return Ok(None);
        }
        let result = if encoding == Encoding::Utf16Le {
            if length < 2 || needle.len() < 2 {
                None
            } else {
                find_utf16(&bytes[..length], &needle, offset, forward)
            }
        } else {
            find_bytes(&bytes[..length], &needle, offset, forward)
        };
        Ok(result
            .and_then(|index| u32::try_from(index).ok())
            .map(|value| jsg::Number::new(f64::from(value))))
    }

    #[jsg_method]
    pub fn swap(
        &self,
        lock: &mut jsg::Lock,
        mut buffer: v8::Local<v8::Uint8Array>,
        size: i32,
    ) -> jsg::Result<()> {
        if buffer.len() <= 1 {
            return Ok(());
        }
        let width = match size {
            16 => 2,
            32 => 4,
            64 => 8,
            _ => jsg_fail_require!(Error, "Unreachable"),
        };
        jsg_require!(buffer.len() % width == 0, Error, "Swap bytes failed");
        // No other buffer reference is live during this mutation.
        // SAFETY: no other Rust reference into this buffer is live.
        let bytes = unsafe { buffer.as_mut_slice(lock) };
        for chunk in bytes.chunks_exact_mut(width) {
            chunk.reverse();
        }
        Ok(())
    }

    #[jsg_method]
    pub fn to_string<'a>(
        &self,
        lock: &mut jsg::Lock,
        bytes: v8::Local<v8::Uint8Array>,
        start: u32,
        end: u32,
        encoding: u8,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        let end = (end as usize).min(bytes.len());
        let start = start as usize;
        if end <= start {
            return Ok(v8::String::empty(lock));
        }
        string_from_bytes(
            lock,
            &bytes.as_slice()[start..end],
            Encoding::from_value(encoding)?,
        )
    }

    #[jsg_method]
    pub fn write(
        &self,
        lock: &mut jsg::Lock,
        mut buffer: v8::Local<v8::Uint8Array>,
        string: v8::Local<v8::String>,
        offset: u32,
        length: u32,
        encoding: u8,
    ) -> jsg::Result<jsg::Number> {
        let offset = offset as usize;
        let length = (length as usize).min(buffer.len().saturating_sub(offset));
        if length == 0 || string.length() == 0 {
            return Ok(jsg::Number::new(0.0));
        }

        let encoding = Encoding::from_value(encoding)?;
        let written = match encoding {
            Encoding::Ascii | Encoding::Latin1 => {
                let count = length.min(usize::try_from(string.length()).unwrap_or_default());
                // SAFETY: no Rust reference into the destination is live, and
                // JavaScript cannot run while V8 copies the string into it.
                let destination = unsafe { buffer.as_mut_slice(lock) };
                string.write_one_byte(
                    lock,
                    0,
                    count as u32,
                    &mut destination[offset..offset + count],
                    v8::WriteFlags::ReplaceInvalidUtf8,
                );
                count
            }
            Encoding::Utf8 => {
                // SAFETY: no Rust reference into the destination is live, and
                // JavaScript cannot run while V8 copies the string into it.
                let destination = unsafe { buffer.as_mut_slice(lock) };
                string.write_utf8(
                    lock,
                    &mut destination[offset..offset + length],
                    v8::WriteFlags::ReplaceInvalidUtf8,
                )
            }
            Encoding::Utf16Le => {
                let count = (length / 2).min(usize::try_from(string.length()).unwrap_or_default());
                let mut units = vec![0; count];
                string.write(
                    lock,
                    0,
                    count as u32,
                    &mut units,
                    v8::WriteFlags::ReplaceInvalidUtf8,
                );
                // A byte slice avoids unaligned u16 writes into a Buffer view.
                // SAFETY: units is owned, and no Rust destination reference is live.
                let destination = unsafe { buffer.as_mut_slice(lock) };
                for (destination, byte) in destination[offset..offset + count * 2]
                    .iter_mut()
                    .zip(units.into_iter().flat_map(u16::to_le_bytes))
                {
                    *destination = byte;
                }
                count * 2
            }
            Encoding::Base64 | Encoding::Base64Url => {
                let string_length = usize::try_from(string.length()).unwrap_or_default();
                let mut text = vec![0; string_length];
                string.write_one_byte(
                    lock,
                    0,
                    string_length as u32,
                    &mut text,
                    v8::WriteFlags::ReplaceInvalidUtf8,
                );
                // nbytes stops decoding as soon as the destination is full.
                // SAFETY: text is owned, and no Rust destination reference is live.
                let destination = unsafe { buffer.as_mut_slice(lock) };
                nbytes::base64_decode_into(&mut destination[offset..offset + length], &text)
            }
            Encoding::Hex => {
                let string_length = usize::try_from(string.length()).unwrap_or_default();
                let mut text = vec![0; string_length];
                string.write_one_byte(
                    lock,
                    0,
                    string_length as u32,
                    &mut text,
                    v8::WriteFlags::ReplaceInvalidUtf8,
                );
                // SAFETY: text is owned, and no Rust destination reference is live.
                let destination = unsafe { buffer.as_mut_slice(lock) };
                decode_hex_into(&mut destination[offset..offset + length], &text)
            }
        };
        let written = u32::try_from(written).unwrap_or(u32::MAX);
        Ok(jsg::Number::new(f64::from(written)))
    }

    #[jsg_method]
    pub fn is_ascii(&self, bytes: v8::Local<v8::Uint8Array>) -> bool {
        bytes.as_slice().is_ascii()
    }

    #[jsg_method]
    pub fn is_utf8(&self, bytes: v8::Local<v8::Uint8Array>) -> bool {
        std::str::from_utf8(bytes.as_slice()).is_ok()
    }

    #[jsg_method]
    pub fn transcode<'a>(
        &self,
        lock: &mut jsg::Lock,
        source: v8::Local<v8::Uint8Array>,
        from_encoding: u8,
        to_encoding: u8,
    ) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
        let from = Encoding::from_value(from_encoding)?;
        let to = Encoding::from_value(to_encoding)?;
        jsg_require!(
            from.can_be_transcoded() && to.can_be_transcoded(),
            Error,
            "Unable to transcode buffer due to unsupported encoding"
        );
        let from = match from {
            Encoding::Ascii => i18n::Encoding::Ascii,
            Encoding::Latin1 => i18n::Encoding::Latin1,
            Encoding::Utf8 => i18n::Encoding::Utf8,
            Encoding::Utf16Le => i18n::Encoding::Utf16Le,
            _ => jsg_fail_require!(
                Error,
                "Unable to transcode buffer due to unsupported encoding"
            ),
        };
        let to = match to {
            Encoding::Ascii => i18n::Encoding::Ascii,
            Encoding::Latin1 => i18n::Encoding::Latin1,
            Encoding::Utf8 => i18n::Encoding::Utf8,
            Encoding::Utf16Le => i18n::Encoding::Utf16Le,
            _ => jsg_fail_require!(
                Error,
                "Unable to transcode buffer due to unsupported encoding"
            ),
        };
        // SAFETY: JSG consumes the returned local before leaving this callback's
        // active V8 HandleScope.
        unsafe { i18n::transcode_buffer(lock, source.as_slice(), from, to) }
    }

    #[jsg_method]
    pub fn decode<'a>(
        &self,
        lock: &mut jsg::Lock,
        bytes: v8::Local<v8::Uint8Array>,
        mut state: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        jsg_require!(
            state.len() == STATE_SIZE,
            TypeError,
            "Invalid StringDecoder"
        );

        // Snapshot both views before mutation. This preserves memmove semantics
        // when the input aliases StringDecoder's state buffer.
        let input = bytes.as_slice().to_vec();
        let mut decoder = state.as_slice().to_vec();
        let encoding = decoder_encoding(&decoder)?;
        if matches!(encoding, Encoding::Ascii | Encoding::Latin1 | Encoding::Hex) {
            return string_from_bytes(lock, &input, encoding);
        }
        if input.is_empty() {
            return Ok(v8::String::empty(lock));
        }

        let mut data_offset = 0;
        let mut remaining = input.len();
        let mut prepend = Vec::new();

        if decoder_missing(&decoder)? > 0 {
            let missing = decoder_missing(&decoder)?;
            let buffered = decoder_buffered(&decoder)?;
            jsg_require!(
                missing + buffered <= INCOMPLETE_END,
                Error,
                "Invalid StringDecoder state"
            );

            if encoding == Encoding::Utf8 {
                for index in 0..remaining.min(missing) {
                    if input[data_offset + index] & 0xc0 != 0x80 {
                        decoder[MISSING_BYTES] = 0;
                        let buffered = decoder_buffered(&decoder)?;
                        decoder[buffered..buffered + index]
                            .copy_from_slice(&input[data_offset..data_offset + index]);
                        decoder[BUFFERED_BYTES] += index as u8;
                        data_offset += index;
                        remaining -= index;
                        break;
                    }
                }
            }

            let missing = decoder_missing(&decoder)?;
            let buffered = decoder_buffered(&decoder)?;
            let found = remaining.min(missing);
            decoder[buffered..buffered + found]
                .copy_from_slice(&input[data_offset..data_offset + found]);
            data_offset += found;
            remaining -= found;
            decoder[MISSING_BYTES] -= found as u8;
            decoder[BUFFERED_BYTES] += found as u8;
            if decoder_missing(&decoder)? == 0 {
                let buffered = decoder_buffered(&decoder)?;
                prepend.extend_from_slice(&decoder[INCOMPLETE_START..buffered]);
                decoder[BUFFERED_BYTES] = 0;
            }
        }

        let body = if remaining > 0 {
            jsg_require!(
                decoder_missing(&decoder)? == 0 && decoder_buffered(&decoder)? == 0,
                Error,
                "Invalid StringDecoder state"
            );
            let data = &input[data_offset..data_offset + remaining];

            if encoding == Encoding::Utf8 && data[remaining - 1] & 0x80 != 0 {
                let mut index = remaining - 1;
                loop {
                    decoder[BUFFERED_BYTES] += 1;
                    if data[index] & 0xc0 == 0x80 {
                        if decoder_buffered(&decoder)? >= 4 || index == 0 {
                            decoder[BUFFERED_BYTES] = 0;
                            break;
                        }
                    } else {
                        decoder[MISSING_BYTES] = if data[index] & 0xe0 == 0xc0 {
                            2
                        } else if data[index] & 0xf0 == 0xe0 {
                            3
                        } else if data[index] & 0xf8 == 0xf0 {
                            4
                        } else {
                            decoder[BUFFERED_BYTES] = 0;
                            break;
                        };
                        if decoder_buffered(&decoder)? >= decoder_missing(&decoder)? {
                            decoder[MISSING_BYTES] = 0;
                            decoder[BUFFERED_BYTES] = 0;
                        }
                        decoder[MISSING_BYTES] -= decoder[BUFFERED_BYTES];
                        break;
                    }
                    index -= 1;
                }
            } else if encoding == Encoding::Utf16Le {
                if remaining % 2 == 1 {
                    decoder[BUFFERED_BYTES] = 1;
                    decoder[MISSING_BYTES] = 1;
                } else if data[remaining - 1] & 0xfc == 0xd8 {
                    decoder[BUFFERED_BYTES] = 2;
                    decoder[MISSING_BYTES] = 2;
                }
            } else if matches!(encoding, Encoding::Base64 | Encoding::Base64Url) {
                decoder[BUFFERED_BYTES] = (remaining % 3) as u8;
                if decoder[BUFFERED_BYTES] > 0 {
                    decoder[MISSING_BYTES] = 3 - decoder[BUFFERED_BYTES];
                }
            }

            let buffered = decoder_buffered(&decoder)?;
            if buffered > 0 {
                remaining -= buffered;
                decoder[INCOMPLETE_START..buffered]
                    .copy_from_slice(&data[remaining..remaining + buffered]);
            }
            data_offset..data_offset + remaining
        } else {
            data_offset..data_offset
        };

        // decoder is an owned snapshot and is the only shared byte reference
        // when the V8 state is updated.
        // SAFETY: decoder is owned and is the only Rust byte reference here.
        unsafe { state.as_mut_slice(lock) }.copy_from_slice(&decoder);

        let prepend = string_from_bytes(lock, &prepend, encoding)?;
        if body.is_empty() {
            return Ok(prepend);
        }
        let body = string_from_bytes(lock, &input[body], encoding)?;
        if prepend.length() == 0 {
            Ok(body)
        } else {
            Ok(v8::String::concat(lock, prepend, body))
        }
    }

    #[jsg_method]
    pub fn flush<'a>(
        &self,
        lock: &mut jsg::Lock,
        mut state: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        jsg_require!(
            state.len() == STATE_SIZE,
            TypeError,
            "Invalid StringDecoder"
        );
        let mut decoder = state.as_slice().to_vec();
        let encoding = decoder_encoding(&decoder)?;
        if matches!(encoding, Encoding::Ascii | Encoding::Hex | Encoding::Latin1) {
            jsg_require!(
                decoder_missing(&decoder)? == 0,
                Error,
                "Invalid StringDecoder state"
            );
            jsg_require!(
                decoder_buffered(&decoder)? == 0,
                Error,
                "Invalid StringDecoder state"
            );
        }
        if encoding == Encoding::Utf16Le && decoder_buffered(&decoder)? % 2 == 1 {
            decoder[MISSING_BYTES] = decoder[MISSING_BYTES].wrapping_sub(1);
            decoder[BUFFERED_BYTES] -= 1;
        }
        let buffered = decoder_buffered(&decoder)?;
        if buffered == 0 {
            // The UTF-16 odd-byte adjustment above mutates the state even when
            // it leaves no buffered character to return.
            // SAFETY: decoder is owned and is the only Rust byte reference here.
            unsafe { state.as_mut_slice(lock) }.copy_from_slice(&decoder);
            return Ok(v8::String::empty(lock));
        }
        let result = string_from_bytes(lock, &decoder[INCOMPLETE_START..buffered], encoding)?;
        decoder[BUFFERED_BYTES] = 0;
        decoder[MISSING_BYTES] = 0;
        // decoder is owned, so it cannot alias the mutable V8 view.
        // SAFETY: decoder is owned and is the only Rust byte reference here.
        unsafe { state.as_mut_slice(lock) }.copy_from_slice(&decoder);
        Ok(result)
    }
}

fn decoder_missing(state: &[u8]) -> jsg::Result<usize> {
    let value = usize::from(state[MISSING_BYTES]);
    jsg_require!(
        value <= INCOMPLETE_END,
        Error,
        "Missing bytes cannot exceed 4"
    );
    Ok(value)
}

fn decoder_buffered(state: &[u8]) -> jsg::Result<usize> {
    let value = usize::from(state[BUFFERED_BYTES]);
    jsg_require!(
        value <= INCOMPLETE_END,
        Error,
        "Buffered bytes cannot exceed 4"
    );
    Ok(value)
}

fn decoder_encoding(state: &[u8]) -> jsg::Result<Encoding> {
    jsg_require!(
        state[ENCODING] <= Encoding::Hex as u8,
        Error,
        "Invalid StringDecoder state"
    );
    Encoding::from_value(state[ENCODING])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_decode_matches_node_truncation() {
        assert_eq!(decode_hex_truncated(b"6162zz63", false).unwrap(), b"ab");
        assert!(decode_hex_truncated(b"123", true).is_err());
    }

    #[test]
    fn base64_encoding_matches_node_variants() {
        assert_eq!(nbytes::base64_encode(b"hello", false), b"aGVsbG8=");
        assert_eq!(nbytes::base64_encode(&[0xfb, 0xff], true), b"-_8");
    }

    #[test]
    fn offset_rules_match_cpp() {
        assert_eq!(index_of_offset(5, -20, 1, true), 0);
        assert_eq!(index_of_offset(5, -20, 1, false), -1);
        assert_eq!(index_of_offset(5, 20, 0, true), 5);
    }
}
