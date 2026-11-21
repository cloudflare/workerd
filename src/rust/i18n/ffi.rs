// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The FFI island of the crate, and the only module that needs `unsafe`. It
//! holds the `#[cxx::bridge]`; the entry point that turns C++'s raw
//! `v8::Isolate*` into a [`jsg::Lock`]; and safe wrappers around simdutf,
//! through `ffi.h`.
#![allow(
    unsafe_code,
    reason = "the crate's FFI island: the cxx bridge, the raw-isolate entry point, and the simdutf calls"
)]

pub use bridge::Encoding;
use jsg::v8;

#[cxx::bridge(namespace = "workerd::rust::i18n")]
mod bridge {
    /// The four encodings `i18n::transcode` supports. Mirrors the
    /// transcodable subset of `workerd::api::node::Encoding`
    /// (`src/workerd/api/node/i18n.h`). `src/workerd/api/node/i18n.c++` maps
    /// into this type through a `fromImpl` overload that rejects the
    /// non-transcodable `BASE64`, `BASE64URL`, and `HEX` variants before ever
    /// calling into Rust.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u8)]
    enum Encoding {
        Ascii,
        Latin1,
        Utf8,
        Utf16Le,
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    unsafe extern "C++" {
        include!("workerd/rust/i18n/ffi.h");

        unsafe fn simdutf_convert_latin1_to_utf16(
            input: *const u8,
            length: usize,
            utf16_output: *mut u16,
        ) -> usize;
        unsafe fn simdutf_ascii_prefix_len(input: *const u8, length: usize) -> usize;
        unsafe fn simdutf_utf8_valid_prefix_len(input: *const u8, length: usize) -> usize;
        unsafe fn simdutf_utf16le_valid_prefix_len(input: *const u16, length: usize) -> usize;
        unsafe fn simdutf_utf8_length_from_latin1(input: *const u8, length: usize) -> usize;
        unsafe fn simdutf_convert_latin1_to_utf8(
            input: *const u8,
            length: usize,
            utf8_output: *mut u8,
        ) -> usize;
        unsafe fn simdutf_utf16_length_from_utf8(input: *const u8, length: usize) -> usize;
        unsafe fn simdutf_convert_utf8_to_utf16le(
            input: *const u8,
            length: usize,
            utf16_output: *mut u16,
        ) -> usize;
        unsafe fn simdutf_utf8_length_from_utf16le(input: *const u16, length: usize) -> usize;
        unsafe fn simdutf_convert_utf16le_to_utf8(
            input: *const u16,
            length: usize,
            utf8_output: *mut u8,
        ) -> usize;
    }

    extern "Rust" {
        /// Transcodes `source` from `from_encoding` to `to_encoding`, for
        /// `workerd::api::node::i18n::transcode`. Returns a `MaybeLocal`
        /// naming a `Uint8Array`, or an empty `MaybeLocal` with a JS exception
        /// already scheduled on `isolate` if transcoding fails.
        ///
        /// # Safety
        /// `isolate` must be a valid pointer to a locked `v8::Isolate` with an
        /// entered context and an active `HandleScope`.
        unsafe fn transcode(
            isolate: *mut Isolate,
            source: &[u8],
            from_encoding: Encoding,
            to_encoding: Encoding,
        ) -> MaybeLocal;
    }
}

/// # Safety
/// `isolate` must be a valid pointer to a locked `v8::Isolate` with an entered
/// context and an active `HandleScope`.
unsafe fn transcode(
    isolate: *mut bridge::Isolate,
    source: &[u8],
    from_encoding: Encoding,
    to_encoding: Encoding,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from this function's own safety contract. The adapter
    // owns the raw-isolate, exception, and local-handle FFI transitions; the
    // feature callback operates only on safe JSG types.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            crate::transcode(lock, source, from_encoding, to_encoding)
        })
    }
}

// simdutf
//
// simdutf's C++ functions, called through the inline forwarders in `ffi.h`,
// declared in the bridge above.
//
// simdutf does not bound its writes by a destination length. Its conversions
// write at most the length its own estimate reports for the same input, valid
// or not: its SIMD kernels stop short of the end of the input by a margin
// that keeps their wide stores inside an estimate-sized buffer ("to avoid
// overruns", in `utf8_to_utf16::validating_transcoder::convert` and
// `avx2_convert_utf16_to_utf8`), and report errors only after converting.
// [`Utf8Source`] and [`Utf16LeSource`] hold each source together with the
// estimate taken from it, so a conversion can check its destination against
// that estimate.
//
// UTF-16 is passed to simdutf as raw bytes reinterpreted as `char16_t*`, and
// so need not be `u16`-aligned. Casting a raw pointer is well-defined in Rust
// regardless of alignment, and no reference to misaligned data is formed on
// this side.

/// Widens Latin-1 `source` into UTF-16LE (written to `target` as raw bytes)
/// via `simdutf::convert_latin1_to_utf16`. Returns the number of UTF-16 code
/// units written, or `None` if `target` cannot hold two bytes per source byte.
#[must_use]
pub fn convert_latin1_to_utf16(source: &[u8], target: &mut [u8]) -> Option<usize> {
    if target.len() / size_of::<u16>() < source.len() {
        return None;
    }
    // SAFETY: every Latin-1 byte widens to exactly one code unit, and `target`
    // holds at least `source.len()` of them.
    Some(unsafe {
        bridge::simdutf_convert_latin1_to_utf16(
            source.as_ptr().cast(),
            source.len(),
            target.as_mut_ptr().cast(),
        )
    })
}

/// The number of leading ASCII bytes in `source`: the position of its first
/// non-ASCII byte, or its length if it has none. Via
/// `simdutf::validate_ascii_with_errors`, which stops scanning at the first
/// 64-byte block that holds a non-ASCII byte and finds the exact position in
/// that block.
pub fn ascii_prefix_len(source: &[u8]) -> usize {
    // SAFETY: pointer and length describe a live slice.
    unsafe { bridge::simdutf_ascii_prefix_len(source.as_ptr(), source.len()) }
}

/// The length of the longest well-formed UTF-8 prefix of `source`, via
/// `simdutf::validate_utf8_with_errors`. It ends before the lead byte of the
/// first ill-formed sequence, so it holds only whole characters.
pub fn utf8_valid_prefix_len(source: &[u8]) -> usize {
    // SAFETY: pointer and length describe a live slice.
    unsafe { bridge::simdutf_utf8_valid_prefix_len(source.as_ptr(), source.len()) }
}

/// The length, in bytes, of the longest well-formed UTF-16LE prefix of the
/// whole code units of `source`, via `simdutf::validate_utf16le_with_errors`.
/// It ends before the first unpaired surrogate, so it holds only whole
/// characters, and a trailing odd byte is never part of it.
pub fn utf16le_valid_prefix_len(source: &[u8]) -> usize {
    // SAFETY: pointer and length describe the whole code units of a live
    // slice.
    let units = unsafe {
        bridge::simdutf_utf16le_valid_prefix_len(
            source.as_ptr().cast(),
            source.len() / size_of::<u16>(),
        )
    };
    units * size_of::<u16>()
}

/// Latin-1 bytes measured by `simdutf::utf8_length_from_latin1`.
pub struct Latin1Source<'a> {
    bytes: &'a [u8],
    utf8_len: usize,
}

impl<'a> Latin1Source<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        // SAFETY: pointer and length describe a live slice.
        let utf8_len =
            unsafe { bridge::simdutf_utf8_length_from_latin1(bytes.as_ptr(), bytes.len()) };
        Self { bytes, utf8_len }
    }

    /// The exact UTF-8 length, in bytes: one per ASCII byte, two per other
    /// byte. Every byte sequence is valid Latin-1.
    pub const fn utf8_len(&self) -> usize {
        self.utf8_len
    }

    /// Converts to UTF-8 via `simdutf::convert_latin1_to_utf8`. Returns the
    /// number of bytes written, or `None` if `target` cannot hold
    /// [`Latin1Source::utf8_len`] bytes.
    pub fn convert_to_utf8(&self, target: &mut [u8]) -> Option<usize> {
        if target.len() < self.utf8_len {
            return None;
        }
        // SAFETY: `target` holds the exact UTF-8 length of these bytes.
        Some(unsafe {
            bridge::simdutf_convert_latin1_to_utf8(
                self.bytes.as_ptr(),
                self.bytes.len(),
                target.as_mut_ptr(),
            )
        })
    }
}

/// UTF-8 bytes measured by `simdutf::utf16_length_from_utf8`.
pub struct Utf8Source<'a> {
    bytes: &'a [u8],
    utf16_len: usize,
}

impl<'a> Utf8Source<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        // SAFETY: pointer and length describe a live slice.
        let utf16_len =
            unsafe { bridge::simdutf_utf16_length_from_utf8(bytes.as_ptr().cast(), bytes.len()) };
        Self { bytes, utf16_len }
    }

    /// simdutf's estimate of the UTF-16 length, in code units. Exact for
    /// valid UTF-8; for invalid UTF-8 it is only the count simdutf reports.
    pub const fn utf16_len(&self) -> usize {
        self.utf16_len
    }

    /// Converts to UTF-16LE (written to `target` as raw bytes) via
    /// `simdutf::convert_utf8_to_utf16le`. Returns the number of code units
    /// written, which is `0` for invalid UTF-8, or `None` if `target` cannot
    /// hold [`Utf8Source::utf16_len`] code units.
    pub fn convert_to_utf16le(&self, target: &mut [u8]) -> Option<usize> {
        if target.len() / 2 < self.utf16_len {
            return None;
        }
        // SAFETY: `target` holds the `utf16_len` code units simdutf estimated
        // for these same bytes, which is the most it writes for them; see the
        // section comment above.
        Some(unsafe {
            bridge::simdutf_convert_utf8_to_utf16le(
                self.bytes.as_ptr().cast(),
                self.bytes.len(),
                target.as_mut_ptr().cast(),
            )
        })
    }
}

/// UTF-16LE code units (as raw bytes) measured by
/// `simdutf::utf8_length_from_utf16le`. A trailing odd byte is ignored.
pub struct Utf16LeSource<'a> {
    bytes: &'a [u8],
    utf8_len: usize,
}

impl<'a> Utf16LeSource<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        // SAFETY: pointer and length describe the whole code units of a live
        // slice.
        let utf8_len = unsafe {
            bridge::simdutf_utf8_length_from_utf16le(
                bytes.as_ptr().cast(),
                bytes.len() / size_of::<u16>(),
            )
        };
        Self { bytes, utf8_len }
    }

    /// simdutf's estimate of the UTF-8 length, in bytes. Exact for valid
    /// UTF-16. An unpaired surrogate has no UTF-8 encoding, and what it counts
    /// for depends on the kernel simdutf selects for the CPU: two bytes, or
    /// three (the size of U+FFFD) in the AVX-512 kernel's path for inputs of
    /// up to 32 code units.
    pub const fn utf8_len(&self) -> usize {
        self.utf8_len
    }

    /// Converts to UTF-8 via `simdutf::convert_utf16le_to_utf8`. Returns the
    /// number of bytes written, which is `0` if the source contains an
    /// unpaired surrogate, or `None` if `target` cannot hold
    /// [`Utf16LeSource::utf8_len`] bytes.
    pub fn convert_to_utf8(&self, target: &mut [u8]) -> Option<usize> {
        if target.len() < self.utf8_len {
            return None;
        }
        // SAFETY: `target` holds the `utf8_len` bytes simdutf estimated for
        // these same code units, which is the most it writes for them; see
        // the section comment above.
        Some(unsafe {
            bridge::simdutf_convert_utf16le_to_utf8(
                self.bytes.as_ptr().cast(),
                self.bytes.len() / size_of::<u16>(),
                target.as_mut_ptr().cast(),
            )
        })
    }
}

#[cfg(test)]
#[path = "ffi-test.rs"]
mod tests;
