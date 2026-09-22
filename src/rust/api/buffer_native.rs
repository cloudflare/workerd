// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `node-internal:buffer_native`: the library primitives used by the TypeScript
//! implementation of `node-internal:buffer` (`src/node/internal/buffer.ts`).
//!
//! `buffer.ts` mirrors `src/workerd/api/node/buffer.c++`. Where the C++ calls
//! into V8, simdutf, nbytes, KJ, or `i18n::transcode`, `buffer.ts` calls the
//! same function through this module instead of reimplementing it. Each method
//! here therefore wraps exactly one such call, with the same options, and
//! contains no Node.js-specific logic of its own.

use jsg::jsg_fail_require;
use jsg::jsg_require;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

#[cxx::bridge(namespace = "workerd::rust::api")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/api/buffer-ffi.h");

        #[cxx_name = "simdutfMaximalBinaryLengthFromBase64"]
        fn simdutf_maximal_binary_length_from_base64(input: &[u8]) -> usize;
        #[cxx_name = "simdutfBase64ToBinary"]
        fn simdutf_base64_to_binary(input: &[u8], output: &mut [u8]) -> usize;
        #[cxx_name = "simdutfBase64LengthFromBinary"]
        fn simdutf_base64_length_from_binary(length: usize) -> usize;
        #[cxx_name = "simdutfBase64UrlLengthFromBinary"]
        fn simdutf_base64_url_length_from_binary(length: usize) -> usize;
        #[cxx_name = "simdutfBinaryToBase64"]
        fn simdutf_binary_to_base64(input: &[u8], output: &mut [u8]) -> usize;
        #[cxx_name = "simdutfBinaryToBase64Url"]
        fn simdutf_binary_to_base64_url(input: &[u8], output: &mut [u8]) -> usize;
        #[cxx_name = "simdutfValidateAscii"]
        fn simdutf_validate_ascii(input: &[u8]) -> bool;
        #[cxx_name = "simdutfValidateUtf8"]
        fn simdutf_validate_utf8(input: &[u8]) -> bool;

        #[cxx_name = "nbytesBase64Decode"]
        fn nbytes_base64_decode(output: &mut [u8], input: &[u8]) -> usize;
        #[cxx_name = "nbytesSwapBytes16"]
        fn nbytes_swap_bytes16(data: &mut [u8]) -> bool;
        #[cxx_name = "nbytesSwapBytes32"]
        fn nbytes_swap_bytes32(data: &mut [u8]) -> bool;
        #[cxx_name = "nbytesSwapBytes64"]
        fn nbytes_swap_bytes64(data: &mut [u8]) -> bool;

        #[cxx_name = "kjEncodeHex"]
        fn kj_encode_hex(input: &[u8], output: &mut [u8]);

        /// # Safety
        /// `isolate` must be a valid pointer to a locked `v8::Isolate` with an
        /// active `HandleScope`.
        #[cxx_name = "i18nTranscode"]
        unsafe fn i18n_transcode(
            isolate: *mut Isolate,
            source: &[u8],
            from_encoding: u8,
            to_encoding: u8,
        ) -> Result<Local>;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
    }
}

/// Returns true if the two views share any bytes.
fn overlaps(a: &v8::Local<v8::Uint8Array>, b: &v8::Local<v8::Uint8Array>) -> bool {
    let a = a.as_slice().as_ptr_range();
    let b = b.as_slice().as_ptr_range();
    a.start < b.end && b.start < a.end
}

/// Runs `f` with a shared view of `input` and a mutable view of `output`.
///
/// If the two views overlap, `input` is copied first so that the Rust
/// references never alias.
fn with_input_output<R>(
    lock: &mut jsg::Lock,
    input: &v8::Local<v8::Uint8Array>,
    output: &mut v8::Local<v8::Uint8Array>,
    f: impl FnOnce(&[u8], &mut [u8]) -> R,
) -> R {
    if overlaps(input, output) {
        let input = input.as_slice().to_vec();
        // SAFETY: `input` is an owned copy, so no other Rust reference into the
        // output view is live. `&mut Lock` prevents JavaScript from running.
        let output = unsafe { output.as_mut_slice(lock) };
        f(&input, output)
    } else {
        let input_slice = input.as_slice();
        // SAFETY: the views do not overlap, so the shared input slice does not
        // alias the mutable output slice. `&mut Lock` prevents JavaScript from
        // running and detaching or resizing either buffer.
        let output = unsafe { output.as_mut_slice(lock) };
        f(input_slice, output)
    }
}

fn write_flags(flags: u8) -> jsg::Result<v8::WriteFlags> {
    Ok(match flags {
        0 => v8::WriteFlags::None,
        1 => v8::WriteFlags::NullTerminate,
        2 => v8::WriteFlags::ReplaceInvalidUtf8,
        3 => v8::WriteFlags::NullTerminateAndReplaceInvalidUtf8,
        _ => jsg_fail_require!(TypeError, "Invalid write flags"),
    })
}

fn string_length(string: &v8::Local<v8::String>) -> usize {
    usize::try_from(string.length()).unwrap_or_default()
}

/// Mirrors the length guard in `jsg::Lock::str()`.
fn require_v8_string_length(length: usize) -> jsg::Result<()> {
    jsg_require!(
        length <= v8::String::MAX_LENGTH as usize,
        RangeError,
        "String is too long for a V8 string"
    );
    Ok(())
}

fn check_string<'a>(
    lock: &mut jsg::Lock,
    maybe: v8::MaybeLocal<'a, v8::String>,
) -> jsg::Result<v8::Local<'a, v8::String>> {
    maybe
        .into_option(lock)
        .ok_or_else(|| jsg::Error::new_range_error("Failed to create string"))
}

// Every length handled here is bounded by the size of an ArrayBuffer or a V8
// string, both of which are exactly representable as f64.
#[expect(
    clippy::cast_precision_loss,
    reason = "buffer and string lengths are below 2^53"
)]
fn usize_to_number(value: usize) -> jsg::Number {
    jsg::Number::new(value as f64)
}

#[jsg_resource]
pub struct BufferNative;

#[jsg_resource]
impl BufferNative {
    #[must_use]
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self)
    }

    // `jsg::JsString::WriteFlags`.
    #[jsg_static_constant]
    pub const WRITE_NONE: u8 = v8::WriteFlags::None as u8;
    #[jsg_static_constant]
    pub const WRITE_NULL_TERMINATION: u8 = v8::WriteFlags::NullTerminate as u8;
    #[jsg_static_constant]
    pub const WRITE_REPLACE_INVALID_UTF8: u8 = v8::WriteFlags::ReplaceInvalidUtf8 as u8;

    // =========================================================================
    // V8 strings

    /// `jsg::JsString::utf8Length()`.
    #[jsg_method]
    pub fn utf8_length(&self, lock: &mut jsg::Lock, string: v8::Local<v8::String>) -> jsg::Number {
        usize_to_number(string.utf8_length(lock))
    }

    /// `jsg::JsString::writeInto(js, kj::ArrayPtr<kj::byte>, flags)`: writes
    /// the low byte of each UTF-16 code unit. Returns the number of bytes
    /// written.
    #[jsg_method]
    pub fn write_one_byte(
        &self,
        lock: &mut jsg::Lock,
        string: v8::Local<v8::String>,
        mut dest: v8::Local<v8::Uint8Array>,
        flags: u8,
    ) -> jsg::Result<jsg::Number> {
        let flags = write_flags(flags)?;
        if dest.is_empty() {
            return Ok(usize_to_number(0));
        }
        let written = dest.len().min(string_length(&string));
        // SAFETY: no other Rust reference into `dest` is live, and `&mut Lock`
        // prevents JavaScript from running while V8 writes into it.
        let buffer = unsafe { dest.as_mut_slice(lock) };
        #[expect(
            clippy::cast_possible_truncation,
            reason = "written is bounded by the string length, an i32"
        )]
        string.write_one_byte(lock, 0, written as u32, buffer, flags);
        Ok(usize_to_number(written))
    }

    /// `jsg::JsString::writeInto(js, kj::ArrayPtr<char>, flags)`: writes
    /// UTF-8. Returns the number of bytes written.
    #[jsg_method]
    pub fn write_utf8(
        &self,
        lock: &mut jsg::Lock,
        string: v8::Local<v8::String>,
        mut dest: v8::Local<v8::Uint8Array>,
        flags: u8,
    ) -> jsg::Result<jsg::Number> {
        let flags = write_flags(flags)?;
        if dest.is_empty() {
            return Ok(usize_to_number(0));
        }
        // SAFETY: no other Rust reference into `dest` is live, and `&mut Lock`
        // prevents JavaScript from running while V8 writes into it.
        let buffer = unsafe { dest.as_mut_slice(lock) };
        Ok(usize_to_number(string.write_utf8(lock, buffer, flags)))
    }

    /// `jsg::JsString::writeInto(js, kj::ArrayPtr<uint16_t>, flags)` over
    /// `dest` reinterpreted as native-endian `uint16_t`s (the last byte of an
    /// odd-length `dest` is not written). Returns the number of code units
    /// written.
    #[jsg_method]
    pub fn write_utf16(
        &self,
        lock: &mut jsg::Lock,
        string: v8::Local<v8::String>,
        mut dest: v8::Local<v8::Uint8Array>,
        flags: u8,
    ) -> jsg::Result<jsg::Number> {
        let flags = write_flags(flags)?;
        let capacity = dest.len() / 2;
        if capacity == 0 {
            return Ok(usize_to_number(0));
        }
        let written = capacity.min(string_length(&string));
        // `dest` may be unaligned, so write through an aligned buffer.
        let mut units = vec![0u16; written];
        #[expect(
            clippy::cast_possible_truncation,
            reason = "written is bounded by the string length, an i32"
        )]
        string.write(lock, 0, written as u32, &mut units, flags);
        // SAFETY: `units` is owned, so no other Rust reference into `dest` is
        // live. `&mut Lock` prevents JavaScript from running.
        let buffer = unsafe { dest.as_mut_slice(lock) };
        for (chunk, unit) in buffer.chunks_exact_mut(2).zip(units) {
            chunk.copy_from_slice(&unit.to_ne_bytes());
        }
        Ok(usize_to_number(written))
    }

    /// `jsg::Lock::str(kj::ArrayPtr<const kj::byte>)`: `NewFromOneByte`.
    #[jsg_method]
    pub fn new_from_one_byte<'a>(
        &self,
        lock: &mut jsg::Lock,
        bytes: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        let bytes = bytes.as_slice();
        require_v8_string_length(bytes.len())?;
        let maybe = v8::String::new_from_one_byte(lock, bytes);
        check_string(lock, maybe)
    }

    /// `jsg::Lock::str(kj::ArrayPtr<const char>)`: `NewFromUtf8`.
    #[jsg_method]
    pub fn new_from_utf8<'a>(
        &self,
        lock: &mut jsg::Lock,
        bytes: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        let bytes = bytes.as_slice();
        require_v8_string_length(bytes.len())?;
        let maybe = v8::String::new_from_utf8(lock, bytes);
        check_string(lock, maybe)
    }

    /// `jsg::Lock::str(kj::ArrayPtr<const uint16_t>)`: `NewFromTwoByte`, over
    /// `bytes` reinterpreted as native-endian `uint16_t`s (a trailing odd byte
    /// is ignored).
    #[jsg_method]
    pub fn new_from_two_byte<'a>(
        &self,
        lock: &mut jsg::Lock,
        bytes: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<v8::Local<'a, v8::String>> {
        // `bytes` may be unaligned, so read through an aligned buffer.
        let units: Vec<u16> = bytes
            .as_slice()
            .chunks_exact(2)
            .map(|pair| u16::from_ne_bytes([pair[0], pair[1]]))
            .collect();
        require_v8_string_length(units.len())?;
        let maybe = v8::String::new_from_two_byte(lock, &units);
        check_string(lock, maybe)
    }

    // =========================================================================
    // simdutf

    /// `simdutf::maximal_binary_length_from_base64`.
    #[jsg_method]
    pub fn simdutf_maximal_binary_length_from_base64(
        &self,
        input: v8::Local<v8::Uint8Array>,
    ) -> jsg::Number {
        usize_to_number(ffi::simdutf_maximal_binary_length_from_base64(
            input.as_slice(),
        ))
    }

    /// `simdutf::base64_to_binary` with
    /// `simdutf::base64_default_or_url_accept_garbage`. Returns the number of
    /// bytes written; the error code is ignored, as in `buffer.c++`.
    #[jsg_method]
    pub fn simdutf_base64_to_binary(
        &self,
        lock: &mut jsg::Lock,
        input: v8::Local<v8::Uint8Array>,
        mut output: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<jsg::Number> {
        let required = ffi::simdutf_maximal_binary_length_from_base64(input.as_slice());
        jsg_require!(
            output.len() >= required,
            RangeError,
            "Output buffer is too small"
        );
        let count = with_input_output(lock, &input, &mut output, |input, output| {
            ffi::simdutf_base64_to_binary(input, output)
        });
        Ok(usize_to_number(count))
    }

    /// `simdutf::base64_length_from_binary` with `simdutf::base64_default`.
    #[jsg_method]
    pub fn simdutf_base64_length_from_binary(&self, length: jsg::Number) -> jsg::Number {
        usize_to_number(ffi::simdutf_base64_length_from_binary(number_to_usize(
            length,
        )))
    }

    /// `simdutf::base64_length_from_binary` with `simdutf::base64_url`.
    #[jsg_method]
    pub fn simdutf_base64_url_length_from_binary(&self, length: jsg::Number) -> jsg::Number {
        usize_to_number(ffi::simdutf_base64_url_length_from_binary(number_to_usize(
            length,
        )))
    }

    /// `simdutf::binary_to_base64` with `simdutf::base64_default`.
    #[jsg_method]
    pub fn simdutf_binary_to_base64(
        &self,
        lock: &mut jsg::Lock,
        input: v8::Local<v8::Uint8Array>,
        mut output: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<jsg::Number> {
        let required = ffi::simdutf_base64_length_from_binary(input.len());
        jsg_require!(
            output.len() >= required,
            RangeError,
            "Output buffer is too small"
        );
        let count = with_input_output(lock, &input, &mut output, |input, output| {
            ffi::simdutf_binary_to_base64(input, output)
        });
        Ok(usize_to_number(count))
    }

    /// `simdutf::binary_to_base64` with `simdutf::base64_url`.
    #[jsg_method]
    pub fn simdutf_binary_to_base64_url(
        &self,
        lock: &mut jsg::Lock,
        input: v8::Local<v8::Uint8Array>,
        mut output: v8::Local<v8::Uint8Array>,
    ) -> jsg::Result<jsg::Number> {
        let required = ffi::simdutf_base64_url_length_from_binary(input.len());
        jsg_require!(
            output.len() >= required,
            RangeError,
            "Output buffer is too small"
        );
        let count = with_input_output(lock, &input, &mut output, |input, output| {
            ffi::simdutf_binary_to_base64_url(input, output)
        });
        Ok(usize_to_number(count))
    }

    /// `simdutf::validate_ascii`.
    #[jsg_method]
    pub fn simdutf_validate_ascii(&self, input: v8::Local<v8::Uint8Array>) -> bool {
        ffi::simdutf_validate_ascii(input.as_slice())
    }

    /// `simdutf::validate_utf8`.
    #[jsg_method]
    pub fn simdutf_validate_utf8(&self, input: v8::Local<v8::Uint8Array>) -> bool {
        ffi::simdutf_validate_utf8(input.as_slice())
    }

    // =========================================================================
    // nbytes

    /// `nbytes::Base64Decode`. Returns the number of bytes written, which is
    /// at most `output.length`.
    #[jsg_method]
    pub fn nbytes_base64_decode(
        &self,
        lock: &mut jsg::Lock,
        mut output: v8::Local<v8::Uint8Array>,
        input: v8::Local<v8::Uint8Array>,
    ) -> jsg::Number {
        let count = with_input_output(lock, &input, &mut output, |input, output| {
            ffi::nbytes_base64_decode(output, input)
        });
        usize_to_number(count)
    }

    /// `nbytes::SwapBytes16`.
    #[jsg_method]
    pub fn nbytes_swap_bytes16(
        &self,
        lock: &mut jsg::Lock,
        mut data: v8::Local<v8::Uint8Array>,
    ) -> bool {
        // SAFETY: no other Rust reference into `data` is live, and `&mut Lock`
        // prevents JavaScript from running.
        ffi::nbytes_swap_bytes16(unsafe { data.as_mut_slice(lock) })
    }

    /// `nbytes::SwapBytes32`.
    #[jsg_method]
    pub fn nbytes_swap_bytes32(
        &self,
        lock: &mut jsg::Lock,
        mut data: v8::Local<v8::Uint8Array>,
    ) -> bool {
        // SAFETY: no other Rust reference into `data` is live, and `&mut Lock`
        // prevents JavaScript from running.
        ffi::nbytes_swap_bytes32(unsafe { data.as_mut_slice(lock) })
    }

    /// `nbytes::SwapBytes64`.
    #[jsg_method]
    pub fn nbytes_swap_bytes64(
        &self,
        lock: &mut jsg::Lock,
        mut data: v8::Local<v8::Uint8Array>,
    ) -> bool {
        // SAFETY: no other Rust reference into `data` is live, and `&mut Lock`
        // prevents JavaScript from running.
        ffi::nbytes_swap_bytes64(unsafe { data.as_mut_slice(lock) })
    }

    // =========================================================================
    // kj

    /// `kj::encodeHex`. Returns the lowercase hex digits as bytes.
    #[jsg_method]
    pub fn kj_encode_hex(&self, input: v8::Local<v8::Uint8Array>) -> Vec<u8> {
        let input = input.as_slice();
        let mut output = vec![0; input.len() * 2];
        ffi::kj_encode_hex(input, &mut output);
        output
    }

    // =========================================================================
    // i18n

    /// `workerd::api::node::i18n::transcode`.
    #[jsg_method]
    pub fn transcode<'a>(
        &self,
        lock: &mut jsg::Lock,
        source: v8::Local<v8::Uint8Array>,
        from_encoding: u8,
        to_encoding: u8,
    ) -> jsg::Result<v8::Local<'a, v8::Uint8Array>> {
        let isolate = lock.isolate();
        // SAFETY: `lock` names a locked isolate with an active HandleScope.
        let local = unsafe {
            ffi::i18n_transcode(
                isolate.as_ffi(),
                source.as_slice(),
                from_encoding,
                to_encoding,
            )
        }
        .map_err(jsg::Error::from)?;
        // SAFETY: `local` is a `Uint8Array` created in the active HandleScope.
        Ok(unsafe { v8::Local::from_ffi(isolate, local) })
    }
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "callers pass non-negative integral buffer lengths"
)]
fn number_to_usize(value: jsg::Number) -> usize {
    value.value() as usize
}
