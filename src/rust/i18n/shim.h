// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// C++ shim exposing the ICU `ucnv_*` primitives and the simdutf conversion
// functions used by `workerd::api::node::i18n::transcode`
// (`src/workerd/api/node/i18n.c++`), so the Rust implementation in `lib.rs`
// calls the exact same codecs as the C++ path rather than reimplementing
// them.

#include <rust/cxx.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// ICU's converter handle (`<unicode/ucnv.h>`). Kept opaque here; the full ICU
// header is only needed in shim.c++.
struct UConverter;

namespace workerd::rust::i18n {

// RAII wrapper around an ICU `UConverter*`, opened by `open_converter()` for
// one of the four transcodable encodings. Exposed to Rust as an opaque C++
// type behind `UniquePtr<Converter>`, so `ucnv_close()` runs in the
// destructor and the converter's lifetime is owned entirely by C++: even if
// Rust code holding the `UniquePtr` panics, unwinding still drops it and runs
// the destructor, unlike a raw `UConverter*` smuggled across the FFI boundary,
// which a panic could leak.
class Converter {
 public:
  explicit Converter(UConverter* conv) noexcept: conv_(conv) {}
  ~Converter() noexcept;
  Converter(const Converter&) = delete;
  Converter& operator=(const Converter&) = delete;

  size_t max_char_size() const;
  size_t min_char_size() const;
  void set_subst_chars(::rust::Str substitute) const;

 private:
  UConverter* conv_;

  friend int64_t convert_ex(const Converter& to,
      const Converter& from,
      ::rust::Slice<const uint8_t> source,
      ::rust::Slice<uint8_t> target);
  friend int64_t from_uchars(
      const Converter& to, ::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target);
};

// Opens an ICU converter for `name` (an ICU encoding name, e.g. "us-ascii").
std::unique_ptr<Converter> open_converter(::rust::Str name);

// `ucnv_convertEx()`-based conversion between two ICU converters, mirroring
// `TranscodeDefault` in `i18n.c++`. `source` and `target` are raw bytes.
// Returns the number of bytes written to `target`, or -1 if ICU reports
// failure.
int64_t convert_ex(const Converter& to,
    const Converter& from,
    ::rust::Slice<const uint8_t> source,
    ::rust::Slice<uint8_t> target);

// `ucnv_fromUChars()`-based conversion from UTF-16LE, mirroring
// `TranscodeFromUTF16` in `i18n.c++`. `source` holds UTF-16LE code units as
// raw bytes (its length must be even); `target` is raw output bytes. Returns
// the number of bytes written to `target`, or -1 if ICU reports failure.
int64_t from_uchars(
    const Converter& to, ::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target);

// simdutf wrappers, mirroring the four `simdutf::*` calls in `i18n.c++`.
// Buffers holding UTF-16LE code units are passed as raw bytes and cast to
// `char16_t*` internally, exactly as the C++ path does via
// `JsUint8Array::asArrayPtr<char16_t>()`.

size_t convert_latin1_to_utf16(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target);
size_t utf16_length_from_utf8(::rust::Slice<const uint8_t> source);
size_t convert_utf8_to_utf16le(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target);
size_t utf8_length_from_utf16le(::rust::Slice<const uint8_t> source);
size_t convert_utf16le_to_utf8(::rust::Slice<const uint8_t> source, ::rust::Slice<uint8_t> target);

}  // namespace workerd::rust::i18n
