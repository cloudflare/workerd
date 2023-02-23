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
  auto slice = bytes.slice(start, end);
  if (slice.size() == 0) return v8::String::Empty(js.v8Isolate);
  switch (getEncoding(encoding)) {
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

uint32_t BufferUtil::write(
    jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    v8::Local<v8::String> string,
    uint32_t offset,
    uint32_t length,
    kj::String encoding) {
  return writeInto(js, buffer, string, offset, length, getEncoding(encoding));
}

}  // namespace workerd::api::node {

