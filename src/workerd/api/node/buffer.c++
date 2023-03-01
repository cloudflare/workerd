// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "buffer.h"
#include "buffer-base64.h"
#include "buffer-string-search.h"
#include <workerd/jsg/buffersource.h>
#include <workerd/api/crypto-impl.h>
#include <kj/encoding.h>
#include <algorithm>

// These are defined by <sys/byteorder.h> or <netinet/in.h> on some systems.
// To avoid warnings, undefine them before redefining them.
#ifdef BSWAP_2
# undef BSWAP_2
#endif
#ifdef BSWAP_4
# undef BSWAP_4
#endif
#ifdef BSWAP_8
# undef BSWAP_8
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#define BSWAP_2(x) _byteswap_ushort(x)
#define BSWAP_4(x) _byteswap_ulong(x)
#define BSWAP_8(x) _byteswap_uint64(x)
#else
#define BSWAP_2(x) ((x) << 8) | ((x) >> 8)
#define BSWAP_4(x)                                                            \
  (((x) & 0xFF) << 24)  |                                                     \
  (((x) & 0xFF00) << 8) |                                                     \
  (((x) >> 8) & 0xFF00) |                                                     \
  (((x) >> 24) & 0xFF)
#define BSWAP_8(x)                                                            \
  (((x) & 0xFF00000000000000ull) >> 56) |                                     \
  (((x) & 0x00FF000000000000ull) >> 40) |                                     \
  (((x) & 0x0000FF0000000000ull) >> 24) |                                     \
  (((x) & 0x000000FF00000000ull) >> 8)  |                                     \
  (((x) & 0x00000000FF000000ull) << 8)  |                                     \
  (((x) & 0x0000000000FF0000ull) << 24) |                                     \
  (((x) & 0x000000000000FF00ull) << 40) |                                     \
  (((x) & 0x00000000000000FFull) << 56)
#endif

namespace workerd::api::node {

const int8_t unbase64_table[256] =
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -1, -1, -2, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, 62, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
  };

namespace {

template <typename T>
void SwapBytes(kj::ArrayPtr<kj::byte> bytes) {
  KJ_DASSERT((bytes.size() % sizeof(T)) == 0);
  uint32_t len = bytes.size() / sizeof(T);
  T* data = reinterpret_cast<T*>(bytes.begin());
  for (uint32_t i = 0; i < len; i++) {
    if constexpr (kj::isSameType<T, uint16_t>()) {
      data[i] = BSWAP_2(data[i]);
    } else if constexpr (kj::isSameType<T, uint32_t>()) {
      data[i] = BSWAP_4(data[i]);
    } else {
      data[i] = BSWAP_8(data[i]);
    }
  }
}

enum class Encoding {
  ASCII,
  LATIN1,
  UTF8,
  UTF16LE,
  BASE64,
  BASE64URL,
  HEX,
};

Encoding getEncoding(kj::StringPtr encoding) {
  if (encoding == "utf8"_kj) {
    return Encoding::UTF8;
  } else if (encoding == "ascii") {
    return Encoding::ASCII;
  } else if (encoding == "latin1") {
    return Encoding::LATIN1;
  } else if (encoding == "utf16le") {
    return Encoding::UTF16LE;
  } else if (encoding == "base64") {
    return Encoding::BASE64;
  } else if (encoding == "base64url") {
    return Encoding::BASE64URL;
  } else if (encoding == "hex") {
    return Encoding::HEX;
  }

  KJ_UNREACHABLE;
}

kj::Maybe<uint> tryFromHexDigit(char c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  } else if ('a' <= c && c <= 'f') {
    return c - ('a' - 10);
  } else if ('A' <= c && c <= 'F') {
    return c - ('A' - 10);
  } else {
    return nullptr;
  }
}

kj::Array<byte> decodeHexTruncated(kj::ArrayPtr<kj::byte> text, bool strict = false) {
  // We do not use kj::decodeHex because we need to match Node.js'
  // behavior of truncating the response at the first invalid hex
  // pair as opposed to just marking that an error happened and
  // trying to continue with the decode.
  if (text.size() % 2 != 0) {
    if (strict) {
      JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
    }
    text = text.slice(0, text.size() - 1);
  }
  kj::Vector vec = kj::Vector<kj::byte>(text.size() / 2);

  for (auto i = 0; i < text.size(); i += 2) {
    byte b = 0;
    KJ_IF_MAYBE(d1, tryFromHexDigit(text[i])) {
      b = *d1 << 4;
    } else {
      if (strict) {
        JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
      }
      break;
    }
    KJ_IF_MAYBE(d2, tryFromHexDigit(text[i+1])) {
      b |= *d2;
    } else {
      if (strict) {
        JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
      }
      break;
    }
    vec.add(b);
  }

  return vec.releaseAsArray();
}

uint32_t writeInto(
    jsg::Lock& js,
    kj::ArrayPtr<kj::byte> buffer,
    v8::Local<v8::String> string,
    uint32_t offset,
    uint32_t length,
    Encoding encoding) {
  auto dest = buffer.slice(offset, kj::min(offset + length, buffer.size()));
  if (dest.size() == 0 || string->Length() == 0) { return 0; }

  int flags = v8::String::HINT_MANY_WRITES_EXPECTED |
              v8::String::NO_NULL_TERMINATION |
              v8::String::REPLACE_INVALID_UTF8;

  switch (encoding) {
    case Encoding::ASCII:
      // Fall-through
    case Encoding::LATIN1: {
      return string->WriteOneByte(js.v8Isolate, dest.begin(), 0, dest.size(), flags);
    }
    case Encoding::UTF8: {
      return string->WriteUtf8(js.v8Isolate, dest.asChars().begin(), dest.size(), nullptr, flags);
    }
    case Encoding::UTF16LE: {
      return string->Write(js.v8Isolate, reinterpret_cast<uint16_t*>(dest.begin()),
                           0, dest.size()/sizeof(uint16_t), flags) * sizeof(uint16_t);
    }
    case Encoding::BASE64:
      // Fall-through
    case Encoding::BASE64URL: {
      auto str = kj::str(string);
      return base64_decode(
          dest.asChars().begin(),
          dest.size(),
          str.begin(),
          str.size());
    }
    case Encoding::HEX: {
      KJ_STACK_ARRAY(kj::byte, buf, string->Length(), 1024, 536870888);
      string->WriteOneByte(js.v8Isolate, buf.begin(), 0, -1,
                           v8::String::NO_NULL_TERMINATION |
                           v8::String::REPLACE_INVALID_UTF8);
      auto bytes = decodeHexTruncated(buf, false);
      auto amountToCopy = kj::min(bytes.size(), dest.size());
      memcpy(dest.begin(), bytes.begin(), amountToCopy);
      return amountToCopy;
    }
  }
  KJ_UNREACHABLE;
}

kj::Array<kj::byte> decodeStringImpl(
    jsg::Lock& js,
    v8::Local<v8::String> string,
    Encoding encoding,
    bool strict = false) {
  if (string->Length() == 0) return kj::Array<kj::byte>();

  switch (encoding) {
    case Encoding::ASCII:
      // Fall-through
    case Encoding::LATIN1: {
      auto dest = kj::heapArray<kj::byte>(string->Length());
      writeInto(js, dest, string, 0, string->Length(), Encoding::LATIN1);
      return kj::mv(dest);
    }
    case Encoding::UTF8: {
      auto dest = kj::heapArray<kj::byte>(string->Utf8Length(js.v8Isolate));
      writeInto(js, dest, string, 0, dest.size(), Encoding::UTF8);
      return kj::mv(dest);
    }
    case Encoding::UTF16LE: {
      auto dest = kj::heapArray<kj::byte>(string->Length() * sizeof(uint16_t));
      writeInto(js, dest, string, 0, dest.size(), Encoding::UTF16LE);
      return kj::mv(dest);
    }
    case Encoding::BASE64:
      // Fall-through
    case Encoding::BASE64URL: {
      // We do not use the kj::String conversion here because inline null-characters
      // need to be ignored.
      KJ_STACK_ARRAY(kj::byte, buf, string->Length(), 1024, 536870888);
      auto len = string->WriteOneByte(js.v8Isolate, buf.begin(), 0, -1,
                                      v8::String::NO_NULL_TERMINATION |
                                      v8::String::REPLACE_INVALID_UTF8);
      auto dest = kj::heapArray<kj::byte>(base64_decoded_size(buf.begin(), len));
      len = base64_decode(
        dest.asChars().begin(),
        dest.size(),
        buf.begin(),
        buf.size());
      return dest.slice(0, len).attach(kj::mv(dest));
    }
    case Encoding::HEX: {
      KJ_STACK_ARRAY(kj::byte, buf, string->Length(), 1024, 536870888);
      string->WriteOneByte(js.v8Isolate, buf.begin(), 0, -1,
                           v8::String::NO_NULL_TERMINATION |
                           v8::String::REPLACE_INVALID_UTF8);
      return decodeHexTruncated(buf, strict);
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

uint32_t BufferUtil::byteLength(jsg::Lock& js, v8::Local<v8::String> str) {
  // Gets the UTF8 byte length for the given input. We use v8::String
  // directly in order to avoid any copying and we just use the V8 API
  // to determine the UTF8 length.
  return str->Utf8Length(js.v8Isolate);
}

int BufferUtil::compare(
    jsg::Lock& js,
    kj::Array<kj::byte> one,
    kj::Array<kj::byte> two,
    jsg::Optional<CompareOptions> maybeOptions) {
  kj::ArrayPtr<kj::byte> ptrOne = one;
  kj::ArrayPtr<kj::byte> ptrTwo = two;

  // The options allow comparing subranges within the two inputs.
  KJ_IF_MAYBE(options, maybeOptions) {
    auto end = options->aEnd.orDefault(ptrOne.size());
    auto start = kj::min(end, options->aStart.orDefault(0));
    ptrOne = ptrOne.slice(start, end);
    end = options->bEnd.orDefault(ptrTwo.size());
    start = kj::min(end, options->bStart.orDefault(0));
    ptrTwo = ptrTwo.slice(start, end);
  }

  size_t toCompare = kj::min(ptrOne.size(), ptrTwo.size());
  auto result = toCompare > 0 ? memcmp(ptrOne.begin(), ptrTwo.begin(), toCompare) : 0;

  if (result == 0) {
    if (ptrOne.size() > ptrTwo.size())
      return 1;
    else if (ptrOne.size() < ptrTwo.size())
      return -1;
    else return 0;
  }

  return result > 0 ? 1 : -1;
}

kj::Array<kj::byte> BufferUtil::concat(
    jsg::Lock& js,
    kj::Array<kj::Array<kj::byte>> list,
    uint32_t length) {
  if (length == 0) return kj::Array<kj::byte>();

  auto dest = kj::heapArray<kj::byte>(length);
  uint32_t offset = 0;
  uint32_t remaining = length;
  auto ptr = dest.begin();
  for (auto& src : list) {
    if (src.size() == 0) continue;
    auto amountToCopy = kj::min(src.size(), remaining);
    std::copy(src.begin(), src.begin() + amountToCopy, ptr + offset);
    offset += amountToCopy;
    remaining -= amountToCopy;
  }
  KJ_DASSERT(offset <= length);
  if (length - offset > 0) {
    memset(ptr + offset, 0, length - offset);
  }

  return kj::mv(dest);
}

kj::Array<kj::byte> BufferUtil::decodeString(
    jsg::Lock& js,
    v8::Local<v8::String> string,
    kj::String encoding) {
  return decodeStringImpl(js, string, getEncoding(encoding));
}

void BufferUtil::fillImpl(
    jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    kj::OneOf<v8::Local<v8::String>, jsg::BufferSource> value,
    uint32_t start,
    uint32_t end,
    jsg::Optional<kj::String> encoding) {
  if (end <= start) return;

  const auto fillFromBytes = [&](kj::ArrayPtr<kj::byte> source) {
    if (source.size() == 0) return;
    auto ptr = buffer.begin() + start;
    auto src = source.begin();
    auto remaining = end - start;
    uint32_t offset = 0;
    while (remaining > 0) {
      auto amountToCopy = kj::min(remaining, source.size());
      std::copy(src, src + amountToCopy, ptr + offset);
      remaining -= amountToCopy;
      offset += amountToCopy;
    }
  };

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(string, v8::Local<v8::String>) {
      auto enc = kj::mv(encoding).orDefault(kj::str("utf8"));
      auto decoded = decodeStringImpl(js, string, getEncoding(enc), true /* strict */);
      fillFromBytes(decoded);
    }
    KJ_CASE_ONEOF(source, jsg::BufferSource) {
      fillFromBytes(source.asArrayPtr());
    }
  }
}

namespace {

// Computes the offset for starting an indexOf or lastIndexOf search.
// Returns either a valid offset in [0...<length - 1>], ie inside the Buffer,
// or -1 to signal that there is no possible match.
int32_t indexOfOffset(size_t length,
                      int32_t offset,
                      int32_t needle_length,
                      bool isForward) {
  int32_t len = static_cast<int32_t>(length);
  if (offset < 0) {
    if (offset + len >= 0) {
      // Negative offsets count backwards from the end of the buffer.
      return len + offset;
    } else if (isForward || needle_length == 0) {
      // indexOf from before the start of the buffer: search the whole buffer.
      return 0;
    } else {
      // lastIndexOf from before the start of the buffer: no match.
      return -1;
    }
  } else {
    if (offset + needle_length <= len) {
      // Valid positive offset.
      return offset;
    } else if (needle_length == 0) {
      // Out of buffer bounds, but empty needle: point to end of buffer.
      return len;
    } else if (isForward) {
      // indexOf from past the end of the buffer: no match.
      return -1;
    } else {
      // lastIndexOf from past the end of the buffer: search the whole buffer.
      return len - 1;
    }
  }
}

jsg::Optional<uint32_t> indexOfBuffer(
    jsg::Lock& js,
    kj::ArrayPtr<kj::byte> hayStack,
    jsg::BufferSource needle,
    int32_t byteOffset,
    kj::String encoding,
    bool isForward) {
  auto enc = getEncoding(encoding);
  auto optOffset = indexOfOffset(hayStack.size(), byteOffset, needle.size(), isForward);

  if (needle.size() == 0) return optOffset;
  if (hayStack.size() == 0 ||
      optOffset <= -1 ||
      (isForward && needle.size() + optOffset > hayStack.size()) ||
      needle.size() > hayStack.size()) {
    return nullptr;
  }

  auto result = hayStack.size();
  if (enc == Encoding::UTF16LE) {
    if (hayStack.size() < 2 || needle.size() < 2) {
      return nullptr;
    }
    result = SearchString(
      reinterpret_cast<const uint16_t*>(hayStack.asChars().begin()),
      hayStack.size() / 2,
      reinterpret_cast<const uint16_t*>(needle.asArrayPtr().asChars().begin()),
      needle.size() / 2,
      optOffset / 2,
      isForward);
    result *= 2;
  } else {
    result = SearchString(
      hayStack.asBytes().begin(),
      hayStack.size(),
      needle.asArrayPtr().asBytes().begin(),
      needle.size(),
      optOffset,
      isForward);
  }

  if (result == hayStack.size()) return nullptr;

  return result;
}

jsg::Optional<uint32_t> indexOfString(
    jsg::Lock& js,
    kj::ArrayPtr<kj::byte> hayStack,
    v8::Local<v8::String> needle,
    int32_t byteOffset,
    kj::String encoding,
    bool isForward) {

  auto enc = getEncoding(encoding);
  auto decodedNeedle = decodeStringImpl(js, needle, enc);

  // Round down to the nearest multiple of 2 in case of UCS2
  auto hayStackLength = enc == Encoding::UTF16LE ? hayStack.size() &~ 1 : hayStack.size();
  auto optOffset = indexOfOffset(hayStackLength, byteOffset, decodedNeedle.size(), isForward);

  if (decodedNeedle.size() == 0) {
    return optOffset;
  }

  if (hayStackLength == 0 ||
      optOffset <= -1 ||
      (isForward && decodedNeedle.size() + optOffset > hayStackLength) ||
      decodedNeedle.size() > hayStackLength) {
    return nullptr;
  }

  auto result = hayStackLength;

  if (enc == Encoding::UTF16LE) {
    if (hayStack.size() < 2 || decodedNeedle.size() < 2) {
      return nullptr;
    }
    result = SearchString(
      reinterpret_cast<const uint16_t*>(hayStack.asChars().begin()),
      hayStack.size() / 2,
      reinterpret_cast<const uint16_t*>(decodedNeedle.asChars().begin()),
      decodedNeedle.size() / 2,
      optOffset / 2,
      isForward);
    result *= 2;
  } else {
    result = SearchString(
      hayStack.asBytes().begin(),
      hayStack.size(),
      decodedNeedle.asBytes().begin(),
      decodedNeedle.size(),
      optOffset,
      isForward);
  }

  if (result == hayStackLength) return nullptr;

  return result;
}

v8::Local<v8::String> toStringImpl(
    jsg::Lock& js,
    kj::ArrayPtr<kj::byte> bytes,
    uint32_t start,
    uint32_t end,
    Encoding encoding) {
  auto slice = bytes.slice(start, end);
  if (slice.size() == 0) return v8::String::Empty(js.v8Isolate);
  switch (encoding) {
    case Encoding::ASCII: {
      // TODO(perf): We can look at making this more performant later.
      // Essentially we have to modify the buffer such that every byte
      // has the highest bit turned off. Whee! Node.js has a faster
      // algorithm that it implements so we can likely adopt that.
      kj::Array<kj::byte> copy = KJ_MAP(b, slice) -> kj::byte { return b & 0x7f; };
      return jsg::v8StrFromLatin1(js.v8Isolate, copy);
    }
    case Encoding::LATIN1: {
      return jsg::v8StrFromLatin1(js.v8Isolate, slice);
    }
    case Encoding::UTF8: {
      return jsg::v8Str(js.v8Isolate, slice.asChars());
    }
    case Encoding::UTF16LE: {
      // TODO(soon): Using just the slice here results in v8 hitting an IsAligned assertion.
      auto data = kj::heapArray<uint16_t>(
          reinterpret_cast<uint16_t*>(slice.begin()), slice.size() / 2);
      return jsg::v8Str<uint16_t>(js.v8Isolate, data);
    }
    case Encoding::BASE64: {
      return jsg::v8Str(js.v8Isolate, kj::encodeBase64(slice));
    }
    case Encoding::BASE64URL: {
      return jsg::v8Str(js.v8Isolate, kj::encodeBase64Url(slice));
    }
    case Encoding::HEX: {
      return jsg::v8Str(js.v8Isolate, kj::encodeHex(slice));
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace

jsg::Optional<uint32_t> BufferUtil::indexOf(
    jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    kj::OneOf<v8::Local<v8::String>, jsg::BufferSource> value,
    int32_t byteOffset,
    kj::String encoding,
    bool isForward) {

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(string, v8::Local<v8::String>) {
      return indexOfString(js, buffer, string, byteOffset, kj::mv(encoding), isForward);
    }
    KJ_CASE_ONEOF(source, jsg::BufferSource) {
      return indexOfBuffer(js, buffer, kj::mv(source), byteOffset, kj::mv(encoding), isForward);
    }
  }
  KJ_UNREACHABLE;
}

void BufferUtil::swap(jsg::Lock& js, kj::Array<kj::byte> buffer, int size) {
  if (buffer.size() <= 1) return;
  switch (size) {
    case 16: return SwapBytes<uint16_t>(buffer);
    case 32: return SwapBytes<uint32_t>(buffer);
    case 64: return SwapBytes<uint64_t>(buffer);
  }
  KJ_UNREACHABLE;
}

v8::Local<v8::String> BufferUtil::toString(
    jsg::Lock& js,
    kj::Array<kj::byte> bytes,
    uint32_t start,
    uint32_t end,
    kj::String encoding) {
  return toStringImpl(js, bytes, start, end, getEncoding(encoding));
}

uint32_t BufferUtil::write(
    jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    v8::Local<v8::String> string,
    uint32_t offset,
    uint32_t length,
    kj::String encoding) {
  return writeInto(js, buffer, string, offset, length, getEncoding(encoding));
}

// ======================================================================================
// StringDecoder
//
// It's helpful to review a bit about how the implementation works here.
//
// StringDecoder is a streaming decoder that ensures that multi-byte characters are correctly
// handled. So, for instance, let's suppose I have the utf8 bytes for a euro symbol (0xe2, 0x82,
// 0xac), but I only get those one at a time... StringDecoder will ensure that those are correctly
// handled over multiple calls to write(...)...
//
//   const sd = new StringDecoder();
//   let results = '';
//   results += sd.write(new Uint8Array([0xe2]));  // results.length === 0
//   results += sd.write(new Uint8Array([0x82]));  // results.length === 0
//   results += sd.write(new Uint8Array([0xac]));  // results.length === 1
//   results += sd.end();
//
// Internally, the decoder allocates a small 7 byte buffer (the state) argument below.
//
// The first four bytes of the state are used to hold partial bytes received on the previous
// write. The fifth byte in state is a count of the number of missing bytes we need to complete
// the character. The sixth byte in state is the number of bytes that have been encoded into the
// first four. The seventh byte in state identifies the Encoding and matches the values of the
// Encoding enum.
//
// So, in our example above, initially the first six bytes of the state are [0x00, 0x00, 0x00,
// 0x00, 0x00, 0x00]
//
// After the first call to write above, the state is updated to: [0xe2, 0x00, 0x00, 0x00, 0x02,
// 0x01]
//
// After the second call to write, the state is updated to: [0xe2, 0x82, 0x00, 0x00, 0x01, 0x02]
//
// After the third call to write, the pending multibyte character is completed, the state becomes:
// [0xe2, 0x82, 0xac, 0x00, 0x00, 0x00] ... while the bytes are still in state, the buffered bytes
// and bytes needed are zeroed out. Since the character is completed on that third write, it is
// included in the returned string.
//
// The implementation here is taken nearly verbatim from Node.js with a few adaptations. The code
// from Node.js has remained largely unchanged for years and is well-proven.

namespace {
inline kj::byte getMissingBytes(kj::ArrayPtr<kj::byte> state) {
  return state[BufferUtil::kMissingBytes];
}

inline kj::byte getBufferedBytes(kj::ArrayPtr<kj::byte> state) {
  return state[BufferUtil::kBufferedBytes];
}

inline kj::byte* getIncompleteCharacterBuffer(kj::ArrayPtr<kj::byte> state) {
  return state.begin() + BufferUtil::kIncompleteCharactersStart;
}

inline Encoding getEncoding(kj::ArrayPtr<kj::byte> state) {
  return static_cast<Encoding>(state[BufferUtil::kEncoding]);
}

v8::Local<v8::String> getBufferedString(jsg::Lock& js, kj::ArrayPtr<kj::byte> state) {
  KJ_ASSERT(getBufferedBytes(state) <= BufferUtil::kIncompleteCharactersEnd);
  auto ret = toStringImpl(js, state,
                          BufferUtil::kIncompleteCharactersStart,
                          BufferUtil::kIncompleteCharactersStart + getBufferedBytes(state),
                          getEncoding(state));
  state[BufferUtil::kBufferedBytes] = 0;
  return ret;
}
}  // namespace

v8::Local<v8::String> BufferUtil::decode(jsg::Lock& js,
                                         kj::Array<kj::byte> bytes,
                                         kj::Array<kj::byte> state) {
  KJ_ASSERT(state.size() == BufferUtil::kSize);
  auto enc = getEncoding(state);
  if (enc == Encoding::ASCII || enc == Encoding::LATIN1 || enc == Encoding::HEX) {
    // For ascii, latin1, and hex, we can just use the regular
    // toString option since there will never be a case where
    // these have left-over characters.
    return toStringImpl(js, bytes, 0, bytes.size(), enc);
  }

  v8::Local<v8::String> prepend;
  v8::Local<v8::String> body;
  auto nread = bytes.size();
  auto data = bytes.begin();
  if (getMissingBytes(state) > 0) {
    KJ_ASSERT(getMissingBytes(state) + getBufferedBytes(state) <=
              BufferUtil::kIncompleteCharactersEnd);
    if (enc == Encoding::UTF8) {
      // For UTF-8, we need special treatment to algin with the V8 decoder:
      // If an incomplete character is found at a chunk boundary, we use
      // its remainder and pass it to V8 as-is.
      for (size_t i = 0; i < nread && i < getMissingBytes(state); ++i) {
        if ((bytes[i] & 0xC0) != 0x80) {
          // This byte is not a continuation byte even though it should have
          // been one. We stop decoding of the incomplete character at this
          // point (but still use the rest of the incomplete bytes from this
          // chunk) and assume that the new, unexpected byte starts a new one.
          state[kMissingBytes] = 0;
          memcpy(getIncompleteCharacterBuffer(state) + getBufferedBytes(state), data, i);
          state[kBufferedBytes] += i;
          data += i;
          nread -= i;
          break;
        }
      }
    }

    size_t found_bytes = std::min(nread, static_cast<size_t>(getMissingBytes(state)));
    memcpy(getIncompleteCharacterBuffer(state) + getBufferedBytes(state), data, found_bytes);
    // Adjust the two buffers.
    data += found_bytes;
    nread -= found_bytes;

    state[kMissingBytes] -= found_bytes;
    state[kBufferedBytes] += found_bytes;

    if (getMissingBytes(state) == 0) {
      // If no more bytes are missing, create a small string that we will later prepend.
      prepend = getBufferedString(js, state);
    }
  }

  if (nread == 0) {
    body = !prepend.IsEmpty() ? prepend : v8::String::Empty(js.v8Isolate);
    prepend = v8::Local<v8::String>();
  } else {
    KJ_ASSERT(getMissingBytes(state) == 0);
    KJ_ASSERT(getBufferedBytes(state) == 0);

    // See whether there is a character that we may have to cut off and
    // finish when receiving the next chunk.
    if (enc == Encoding::UTF8 && data[nread - 1] & 0x80) {
      // This is UTF-8 encoded data and we ended on a non-ASCII UTF-8 byte.
      // This means we'll need to figure out where the character to which
      // the byte belongs begins.
      for (size_t i = nread - 1; ; --i) {
        KJ_ASSERT(i < nread);
        state[kBufferedBytes]++;
        if ((data[i] & 0xC0) == 0x80) {
          // This byte does not start a character (a "trailing" byte).
          if (state[kBufferedBytes] >= 4 || i == 0) {
            // We either have more then 4 trailing bytes (which means
            // the current character would not be inside the range for
            // valid Unicode, and in particular cannot be represented
            // through JavaScript's UTF-16-based approach to strings), or the
            // current buffer does not contain the start of an UTF-8 character
            // at all. Either way, this is invalid UTF8 and we can just
            // let the engine's decoder handle it.
            state[kBufferedBytes] = 0;
            break;
          }
        } else {
          // Found the first byte of a UTF-8 character. By looking at the
          // upper bits we can tell how long the character *should* be.
          if ((data[i] & 0xE0) == 0xC0) {
            state[kMissingBytes] = 2;
          } else if ((data[i] & 0xF0) == 0xE0) {
            state[kMissingBytes] = 3;
          } else if ((data[i] & 0xF8) == 0xF0) {
            state[kMissingBytes] = 4;
          } else {
            // This lead byte would indicate a character outside of the
            // representable range.
            state[kBufferedBytes] = 0;
            break;
          }

          if (getBufferedBytes(state) >= getMissingBytes(state)) {
            // Received more or exactly as many trailing bytes than the lead
            // character would indicate. In the "==" case, we have valid
            // data and don't need to slice anything off;
            // in the ">" case, this is invalid UTF-8 anyway.
            state[kMissingBytes] = 0;
            state[kBufferedBytes] = 0;
          }

          state[kMissingBytes] -= state[kBufferedBytes];
          break;
        }
      }
    } else if (enc == Encoding::UTF16LE) {
      if ((nread % 2) == 1) {
        // We got half a codepoint, and need the second byte of it.
        state[kBufferedBytes] = 1;
        state[kMissingBytes] = 1;
      } else if ((data[nread - 1] & 0xFC) == 0xD8) {
        // Half a split UTF-16 character.
        state[kBufferedBytes] = 2;
        state[kMissingBytes] = 2;
      }
    } else if (enc == Encoding::BASE64 || enc == Encoding::BASE64URL) {
      state[kBufferedBytes] = nread % 3;
      if (state[kBufferedBytes] > 0)
        state[kMissingBytes] = 3 - getBufferedBytes(state);
    }

    if (getBufferedBytes(state) > 0) {
      // Copy the requested number of buffered bytes from the end of the
      // input into the incomplete character buffer.
      nread -= getBufferedBytes(state);
      memcpy(getIncompleteCharacterBuffer(state), data + nread, getBufferedBytes(state));
    }

    if (nread > 0) {
      body = toStringImpl(js, kj::ArrayPtr<kj::byte>(data, data + nread), 0, nread, enc);
    } else {
      body = v8::String::Empty(js.v8Isolate);
    }
  }

  if (prepend.IsEmpty()) {
    return body;
  } else {
    return v8::String::Concat(js.v8Isolate, prepend, body);
  }

  return v8::String::Empty(js.v8Isolate);
}

v8::Local<v8::String> BufferUtil::flush(jsg::Lock& js, kj::Array<kj::byte> state) {
  KJ_ASSERT(state.size() == BufferUtil::kSize);
  auto enc = getEncoding(state);
  if (enc == Encoding::ASCII || enc == Encoding::HEX || enc == Encoding::LATIN1) {
    KJ_ASSERT(getMissingBytes(state) == 0);
    KJ_ASSERT(getBufferedBytes(state) == 0);
  }

  if (enc == Encoding::UTF16LE && getBufferedBytes(state) % 2 == 1) {
    // Ignore a single trailing byte, like the JS decoder does.
    state[kMissingBytes]--;
    state[kBufferedBytes]--;
  }

  if (getBufferedBytes(state) == 0) {
    return v8::String::Empty(js.v8Isolate);
  }

  auto ret = getBufferedString(js, state);
  state[kMissingBytes] = 0;

  return ret;
}

}  // namespace workerd::api::node {

