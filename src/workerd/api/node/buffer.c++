// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

#include "buffer.h"

#include "buffer-string-search.h"
#include "nbytes.h"
#include "simdutf.h"

#include <workerd/jsg/jsg.h>

#include <kj/array.h>
#include <kj/encoding.h>

#include <algorithm>

namespace workerd::api::node {

namespace {

kj::Maybe<uint> tryFromHexDigit(char c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  } else if ('a' <= c && c <= 'f') {
    return c - ('a' - 10);
  } else if ('A' <= c && c <= 'F') {
    return c - ('A' - 10);
  }
  return kj::none;
}

jsg::BackingStore decodeHexTruncated(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> text, bool strict = false) {
  // We do not use kj::decodeHex because we need to match Node.js'
  // behavior of truncating the response at the first invalid hex
  // pair as opposed to just marking that an error happened and
  // trying to continue with the decode.
  if (text.size() % 2 != 0) {
    if (strict) {
      JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
    }
    text = text.first(text.size() - 1);
  }
  auto vec = jsg::BackingStore::alloc<v8::Uint8Array>(js, text.size() / 2);
  auto ptr = vec.asArrayPtr();
  size_t len = 0;

  for (size_t i = 0; i < text.size(); i += 2) {
    kj::byte b = 0;
    KJ_IF_SOME(d1, tryFromHexDigit(text[i])) {
      b = d1 << 4;
    } else {
      if (strict) {
        JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
      }
      break;
    }
    KJ_IF_SOME(d2, tryFromHexDigit(text[i + 1])) {
      b |= d2;
    } else {
      if (strict) {
        JSG_FAIL_REQUIRE(TypeError, "The text is not valid hex");
      }
      break;
    }
    ptr[len++] = b;
  }

  vec.limit(len);
  return kj::mv(vec);
}

uint32_t writeInto(jsg::Lock& js,
    kj::ArrayPtr<kj::byte> buffer,
    jsg::JsString string,
    uint32_t offset,
    uint32_t length,
    Encoding encoding) {
  auto dest = buffer.slice(offset, kj::min(offset + length, buffer.size()));
  if (dest.size() == 0 || string.length(js) == 0) {
    return 0;
  }

  static constexpr jsg::JsString::WriteOptions flags = static_cast<jsg::JsString::WriteOptions>(
      jsg::JsString::NO_NULL_TERMINATION | jsg::JsString::REPLACE_INVALID_UTF8);

  switch (encoding) {
    case Encoding::ASCII:
      // Fall-through
    case Encoding::LATIN1: {
      auto result = string.writeInto(js, dest, flags);
      return result.written;
    }
    case Encoding::UTF8: {
      auto result = string.writeInto(js, dest.asChars(), flags);
      return result.written;
    }
    case Encoding::UTF16LE: {
#if __has_feature(undefined_behavior_sanitizer)
      // UBSan warns about unaligned writes, but this can be hard to avoid if dest is unaligned.
      // Use temp variable to perform aligned write instead.
      kj::Array<uint16_t> tmpBuf = kj::heapArray<uint16_t>(dest.size() / sizeof(uint16_t));
      auto result = string.writeInto(js, tmpBuf, flags);
      kj::ArrayPtr<uint16_t> buf(reinterpret_cast<uint16_t*>(dest.begin()), result.written);
      buf.copyFrom(tmp);
#else
      kj::ArrayPtr<uint16_t> buf(
          reinterpret_cast<uint16_t*>(dest.begin()), dest.size() / sizeof(uint16_t));
      auto result = string.writeInto(js, buf, flags);
#endif
      return result.written * sizeof(uint16_t);
    }
    case Encoding::BASE64:
      // Fall-through
    case Encoding::BASE64URL: {
      auto str = kj::str(string);
      return nbytes::Base64Decode(dest.asChars().begin(), dest.size(), str.begin(), str.size());
    }
    case Encoding::HEX: {
      KJ_STACK_ARRAY(kj::byte, buf, string.length(js), 1024, 536870888);
      static constexpr jsg::JsString::WriteOptions options =
          static_cast<jsg::JsString::WriteOptions>(
              jsg::JsString::NO_NULL_TERMINATION | jsg::JsString::REPLACE_INVALID_UTF8);
      string.writeInto(js, buf, options);
      auto backing = decodeHexTruncated(js, buf, false);
      auto bytes = backing.asArrayPtr();
      auto amountToCopy = kj::min(bytes.size(), dest.size());
      dest.first(amountToCopy).copyFrom(bytes.first(amountToCopy));
      return amountToCopy;
    }
    default:
      KJ_UNREACHABLE;
  }
}

jsg::BackingStore decodeStringImpl(
    jsg::Lock& js, const jsg::JsString& string, Encoding encoding, bool strict = false) {
  auto length = string.length(js);
  if (length == 0) return jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);

  static constexpr jsg::JsString::WriteOptions options = static_cast<jsg::JsString::WriteOptions>(
      jsg::JsString::NO_NULL_TERMINATION | jsg::JsString::REPLACE_INVALID_UTF8);

  switch (encoding) {
    case Encoding::ASCII:
      // Fall-through
    case Encoding::LATIN1: {
      auto dest = jsg::BackingStore::alloc<v8::Uint8Array>(js, length);
      writeInto(js, dest, string, 0, dest.size(), Encoding::LATIN1);
      return kj::mv(dest);
    }
    case Encoding::UTF8: {
      auto dest = jsg::BackingStore::alloc<v8::Uint8Array>(js, string.utf8Length(js));
      writeInto(js, dest, string, 0, dest.size(), Encoding::UTF8);
      return kj::mv(dest);
    }
    case Encoding::UTF16LE: {
      auto dest = jsg::BackingStore::alloc<v8::Uint8Array>(js, length * sizeof(uint16_t));
      writeInto(js, dest, string, 0, dest.size(), Encoding::UTF16LE);
      return kj::mv(dest);
    }
    case Encoding::BASE64:
      // Fall-through
    case Encoding::BASE64URL: {
      // TODO(soon): Use simdutf for faster decoding for BASE64 and BASE64URL.
      // We do not use the kj::String conversion here because inline null-characters
      // need to be ignored.
      KJ_STACK_ARRAY(kj::byte, buf, length, 1024, 536870888);
      auto result = string.writeInto(js, buf, options);
      auto len = result.written;
      auto dest =
          jsg::BackingStore::alloc<v8::Uint8Array>(js, nbytes::Base64DecodedSize(buf.begin(), len));
      len = nbytes::Base64Decode(
          dest.asArrayPtr<char>().begin(), dest.size(), buf.begin(), buf.size());
      dest.limit(len);
      return kj::mv(dest);
    }
    case Encoding::HEX: {
      KJ_STACK_ARRAY(kj::byte, buf, length, 1024, 536870888);
      string.writeInto(js, buf, options);
      return decodeHexTruncated(js, buf, strict);
    }
    default:
      KJ_UNREACHABLE;
  }
}
}  // namespace

uint32_t BufferUtil::byteLength(jsg::Lock& js, jsg::JsString str) {
  return str.utf8Length(js);
}

int BufferUtil::compare(jsg::Lock& js,
    kj::Array<kj::byte> one,
    kj::Array<kj::byte> two,
    jsg::Optional<CompareOptions> maybeOptions) {
  kj::ArrayPtr<kj::byte> ptrOne = one;
  kj::ArrayPtr<kj::byte> ptrTwo = two;

  // The options allow comparing subranges within the two inputs.
  KJ_IF_SOME(options, maybeOptions) {
    auto end = options.aEnd.orDefault(ptrOne.size());
    auto start = kj::min(end, options.aStart.orDefault(0));
    ptrOne = ptrOne.slice(start, end);
    end = options.bEnd.orDefault(ptrTwo.size());
    start = kj::min(end, options.bStart.orDefault(0));
    ptrTwo = ptrTwo.slice(start, end);
  }

  size_t toCompare = kj::min(ptrOne.size(), ptrTwo.size());
  auto result = toCompare > 0 ? memcmp(ptrOne.begin(), ptrTwo.begin(), toCompare) : 0;

  if (result == 0) {
    if (ptrOne.size() > ptrTwo.size())
      return 1;
    else if (ptrOne.size() < ptrTwo.size())
      return -1;
    else
      return 0;
  }

  return result > 0 ? 1 : -1;
}

jsg::BufferSource BufferUtil::concat(
    jsg::Lock& js, kj::Array<kj::Array<kj::byte>> list, uint32_t length) {
  if (length == 0) {
    auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, 0);
    return jsg::BufferSource(js, kj::mv(backing));
  }

  // The Node.js Buffer.concat is interesting in that it doesn't just append
  // the buffers together as is. The length parameter is used to determine the
  // length of the result which can be lesser or greater than the actual
  // combined lengths of the inputs. If the length is lesser, the result will
  // be a truncated version of the combined buffers. If the length is greater,
  // the result will be the combined buffers with the remaining space filled
  // with zeroes.

  auto dest = jsg::BackingStore::alloc<v8::Uint8Array>(js, length);
  auto view = dest.asArrayPtr();

  for (auto& src: list) {
    if (src.size() == 0) continue;
    // The amount to copy is the lesser of the remaining space in the destination or
    // the size of the chunk we're copying.
    auto amountToCopy = kj::min(src.size(), view.size());
    view.first(amountToCopy).copyFrom(src.first(amountToCopy));
    view = view.slice(amountToCopy);
    // If there's no more space in the destination, we're done.
    if (view == nullptr) {
      return jsg::BufferSource(js, kj::mv(dest));
    }
  }

  // Fill any remaining space in the destination with zeroes.
  view.fill(0);
  return jsg::BufferSource(js, kj::mv(dest));
}

jsg::BufferSource BufferUtil::decodeString(
    jsg::Lock& js, jsg::JsString string, EncodingValue encoding) {
  return jsg::BufferSource(js, decodeStringImpl(js, string, static_cast<Encoding>(encoding)));
}

void BufferUtil::fillImpl(jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    kj::OneOf<jsg::JsString, jsg::BufferSource> value,
    uint32_t start,
    uint32_t end,
    jsg::Optional<EncodingValue> encoding) {
  end = kj::min(end, buffer.size());
  if (end <= start) return;

  auto ptr = buffer.slice(start, end);
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(string, jsg::JsString) {
      auto enc = encoding.orDefault(Encoding::UTF8);
      auto decoded = decodeStringImpl(js, string, static_cast<Encoding>(enc), true /* strict */);
      if (decoded.size() == 0) {
        ptr.fill(0);
        return;
      }
      ptr.fill(decoded);
    }
    KJ_CASE_ONEOF(source, jsg::BufferSource) {
      if (source.size() == 0) {
        ptr.fill(0);
        return;
      }
      ptr.fill(source.asArrayPtr());
    }
  }
}

namespace {

// Computes the offset for starting an indexOf or lastIndexOf search.
// Returns either a valid offset in [0...<length - 1>], ie inside the Buffer,
// or -1 to signal that there is no possible match.
int32_t indexOfOffset(size_t length, int32_t offset, int32_t needle_length, bool isForward) {
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
    // cast to int64_t to avoid overflow.
    if ((int64_t)offset + needle_length <= len) {
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

jsg::Optional<uint32_t> indexOfBuffer(jsg::Lock& js,
    kj::ArrayPtr<kj::byte> hayStack,
    jsg::BufferSource needle,
    int32_t byteOffset,
    EncodingValue encoding,
    bool isForward) {
  auto enc = static_cast<Encoding>(encoding);
  auto optOffset = indexOfOffset(hayStack.size(), byteOffset, needle.size(), isForward);

  if (needle.size() == 0) return optOffset;
  if (hayStack.size() == 0 || optOffset <= -1 ||
      (isForward && needle.size() + optOffset > hayStack.size()) ||
      needle.size() > hayStack.size()) {
    return kj::none;
  }

  auto result = hayStack.size();
  if (enc == Encoding::UTF16LE) {
    if (hayStack.size() < 2 || needle.size() < 2) {
      return kj::none;
    }
    result = SearchString(reinterpret_cast<const uint16_t*>(hayStack.asChars().begin()),
        hayStack.size() / 2,
        reinterpret_cast<const uint16_t*>(needle.asArrayPtr().asChars().begin()), needle.size() / 2,
        optOffset / 2, isForward);
    result *= 2;
  } else {
    result = SearchString(hayStack.asBytes().begin(), hayStack.size(),
        needle.asArrayPtr().asBytes().begin(), needle.size(), optOffset, isForward);
  }

  if (result == hayStack.size()) return kj::none;

  return result;
}

jsg::Optional<uint32_t> indexOfString(jsg::Lock& js,
    kj::ArrayPtr<kj::byte> hayStack,
    const jsg::JsString& needle,
    int32_t byteOffset,
    EncodingValue encoding,
    bool isForward) {

  auto enc = static_cast<Encoding>(encoding);
  auto decodedNeedle = decodeStringImpl(js, needle, enc);

  // Round down to the nearest multiple of 2 in case of UCS2
  auto hayStackLength = enc == Encoding::UTF16LE ? hayStack.size() & ~1 : hayStack.size();
  auto optOffset = indexOfOffset(hayStackLength, byteOffset, decodedNeedle.size(), isForward);

  if (decodedNeedle.size() == 0) {
    return optOffset;
  }

  if (hayStackLength == 0 || optOffset <= -1 ||
      (isForward && decodedNeedle.size() + optOffset > hayStackLength) ||
      decodedNeedle.size() > hayStackLength) {
    return kj::none;
  }

  auto result = hayStackLength;

  if (enc == Encoding::UTF16LE) {
    if (hayStack.size() < 2 || decodedNeedle.size() < 2) {
      return kj::none;
    }
    result = SearchString(reinterpret_cast<const uint16_t*>(hayStack.asChars().begin()),
        hayStack.size() / 2,
        reinterpret_cast<const uint16_t*>(decodedNeedle.asArrayPtr<char>().begin()),
        decodedNeedle.size() / 2, optOffset / 2, isForward);
    result *= 2;
  } else {
    result = SearchString(hayStack.asBytes().begin(), hayStack.size(),
        decodedNeedle.asArrayPtr().begin(), decodedNeedle.size(), optOffset, isForward);
  }

  if (result == hayStackLength) return kj::none;

  return result;
}

jsg::JsString toStringImpl(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> bytes, uint32_t start, uint32_t end, Encoding encoding) {
  if (end < start) end = start;
  auto slice = bytes.slice(start, end);
  if (slice.size() == 0) return js.str();
  switch (encoding) {
    case Encoding::ASCII: {
      // TODO(perf): We can look at making this more performant later.
      // Essentially we have to modify the buffer such that every byte
      // has the highest bit turned off. Whee! Node.js has a faster
      // algorithm that it implements so we can likely adopt that.
      kj::Array<kj::byte> copy = KJ_MAP(b, slice) -> kj::byte { return b & 0x7f; };
      return js.str(copy);
    }
    case Encoding::LATIN1: {
      return js.str(slice);
    }
    case Encoding::UTF8: {
      return js.str(slice.asChars());
    }
    case Encoding::UTF16LE: {
      // TODO(soon): Using just the slice here results in v8 hitting an IsAligned assertion.
      auto data =
          kj::heapArray<uint16_t>(reinterpret_cast<uint16_t*>(slice.begin()), slice.size() / 2);
      return js.str(data);
    }
    case Encoding::BASE64: {
      size_t length = simdutf::base64_length_from_binary(slice.size());
      auto out = kj::heapArray<kj::byte>(length);
      simdutf::binary_to_base64(reinterpret_cast<const char*>(slice.begin()), slice.size(),
          reinterpret_cast<char*>(out.begin()));
      return js.str(out);
    }
    case Encoding::BASE64URL: {
      auto options = simdutf::base64_url;
      size_t length = simdutf::base64_length_from_binary(slice.size(), options);
      auto out = kj::heapArray<kj::byte>(length);
      simdutf::binary_to_base64(reinterpret_cast<const char*>(slice.begin()), slice.size(),
          reinterpret_cast<char*>(out.begin()), options);
      return js.str(out);
    }
    case Encoding::HEX: {
      return js.str(kj::encodeHex(slice));
    }
    default:
      KJ_UNREACHABLE;
  }
}

}  // namespace

jsg::Optional<uint32_t> BufferUtil::indexOf(jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    kj::OneOf<jsg::JsString, jsg::BufferSource> value,
    int32_t byteOffset,
    EncodingValue encoding,
    bool isForward) {

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(string, jsg::JsString) {
      return indexOfString(js, buffer, string, byteOffset, encoding, isForward);
    }
    KJ_CASE_ONEOF(source, jsg::BufferSource) {
      return indexOfBuffer(js, buffer, kj::mv(source), byteOffset, encoding, isForward);
    }
  }
  KJ_UNREACHABLE;
}

void BufferUtil::swap(jsg::Lock& js, kj::Array<kj::byte> buffer, int size) {
  if (buffer.size() <= 1) return;
  switch (size) {
    case 16: {
      JSG_REQUIRE(
          nbytes::SwapBytes16(buffer.asChars().begin(), buffer.size()), Error, "Swap bytes failed");
      break;
    }
    case 32: {
      JSG_REQUIRE(
          nbytes::SwapBytes32(buffer.asChars().begin(), buffer.size()), Error, "Swap bytes failed");
      break;
    }
    case 64: {
      JSG_REQUIRE(
          nbytes::SwapBytes64(buffer.asChars().begin(), buffer.size()), Error, "Swap bytes failed");
      break;
    }
    default:
      JSG_FAIL_REQUIRE(Error, "Unreachable");
  }
}

jsg::JsString BufferUtil::toString(jsg::Lock& js,
    kj::Array<kj::byte> bytes,
    uint32_t start,
    uint32_t end,
    EncodingValue encoding) {
  return toStringImpl(js, bytes, start, end, static_cast<Encoding>(encoding));
}

uint32_t BufferUtil::write(jsg::Lock& js,
    kj::Array<kj::byte> buffer,
    jsg::JsString string,
    uint32_t offset,
    uint32_t length,
    EncodingValue encoding) {
  return writeInto(js, buffer, string, offset, length, static_cast<Encoding>(encoding));
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
  JSG_REQUIRE(state[BufferUtil::kMissingBytes] <= BufferUtil::kIncompleteCharactersEnd, Error,
      "Missing bytes cannot exceed 4");
  return state[BufferUtil::kMissingBytes];
}

inline kj::byte getBufferedBytes(kj::ArrayPtr<kj::byte> state) {
  JSG_REQUIRE(state[BufferUtil::kBufferedBytes] <= BufferUtil::kIncompleteCharactersEnd, Error,
      "Buffered bytes cannot exceed 4");
  return state[BufferUtil::kBufferedBytes];
}

inline kj::byte* getIncompleteCharacterBuffer(kj::ArrayPtr<kj::byte> state) {
  return state.begin() + BufferUtil::kIncompleteCharactersStart;
}

inline Encoding getEncoding(kj::ArrayPtr<kj::byte> state) {
  JSG_REQUIRE(state[BufferUtil::kEncoding] <= static_cast<kj::byte>(Encoding::HEX), Error,
      "Invalid StringDecoder state");
  return static_cast<Encoding>(state[BufferUtil::kEncoding]);
}

jsg::JsString getBufferedString(jsg::Lock& js, kj::ArrayPtr<kj::byte> state) {
  JSG_REQUIRE(getBufferedBytes(state) <= BufferUtil::kIncompleteCharactersEnd, Error,
      "Invalid StringDecoder state");
  auto ret = toStringImpl(js, state, BufferUtil::kIncompleteCharactersStart,
      BufferUtil::kIncompleteCharactersStart + getBufferedBytes(state), getEncoding(state));
  state[BufferUtil::kBufferedBytes] = 0;
  return ret;
}
}  // namespace

jsg::JsString BufferUtil::decode(
    jsg::Lock& js, kj::Array<kj::byte> bytes, kj::Array<kj::byte> state) {
  JSG_REQUIRE(state.size() == BufferUtil::kSize, TypeError, "Invalid StringDecoder");
  auto enc = getEncoding(state);
  if (enc == Encoding::ASCII || enc == Encoding::LATIN1 || enc == Encoding::HEX) {
    // For ascii, latin1, and hex, we can just use the regular
    // toString option since there will never be a case where
    // these have left-over characters.
    return toStringImpl(js, bytes, 0, bytes.size(), enc);
  }

  jsg::JsString prepend = js.str();
  jsg::JsString body = js.str();
  auto nread = bytes.size();
  auto data = bytes.begin();
  if (getMissingBytes(state) > 0) {
    JSG_REQUIRE(
        getMissingBytes(state) + getBufferedBytes(state) <= BufferUtil::kIncompleteCharactersEnd,
        Error, "Invalid StringDecoder state");
    if (enc == Encoding::UTF8) {
      // For UTF-8, we need special treatment to align with the V8 decoder:
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
    body = prepend.length(js) ? prepend : js.str();
    prepend = js.str();
  } else {
    JSG_REQUIRE(getMissingBytes(state) == 0, Error, "Invalid StringDecoder state");
    JSG_REQUIRE(getBufferedBytes(state) == 0, Error, "Invalid StringDecoder state");

    // See whether there is a character that we may have to cut off and
    // finish when receiving the next chunk.
    if (enc == Encoding::UTF8 && data[nread - 1] & 0x80) {
      // This is UTF-8 encoded data and we ended on a non-ASCII UTF-8 byte.
      // This means we'll need to figure out where the character to which
      // the byte belongs begins.
      for (size_t i = nread - 1;; --i) {
        JSG_REQUIRE(i < nread, Error, "Invalid StringDecoder state");
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
      if (state[kBufferedBytes] > 0) state[kMissingBytes] = 3 - getBufferedBytes(state);
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
      body = js.str();
    }
  }

  if (prepend.length(js) == 0) {
    return body;
  } else {
    return jsg::JsString::concat(js, prepend, body);
  }

  return js.str();
}

jsg::JsString BufferUtil::flush(jsg::Lock& js, kj::Array<kj::byte> state) {
  JSG_REQUIRE(state.size() == BufferUtil::kSize, TypeError, "Invalid StringDecoder");
  auto enc = getEncoding(state);
  if (enc == Encoding::ASCII || enc == Encoding::HEX || enc == Encoding::LATIN1) {
    JSG_REQUIRE(getMissingBytes(state) == 0, Error, "Invalid StringDecoder state");
    JSG_REQUIRE(getBufferedBytes(state) == 0, Error, "Invalid StringDecoder state");
  }

  if (enc == Encoding::UTF16LE && getBufferedBytes(state) % 2 == 1) {
    // Ignore a single trailing byte, like the JS decoder does.
    state[kMissingBytes]--;
    state[kBufferedBytes]--;
  }

  if (getBufferedBytes(state) == 0) {
    return js.str();
  }

  auto ret = getBufferedString(js, state);
  state[kMissingBytes] = 0;

  return ret;
}

bool BufferUtil::isAscii(kj::Array<kj::byte> buffer) {
  if (buffer.size() == 0) return true;
  return simdutf::validate_ascii(buffer.asChars().begin(), buffer.size());
}

bool BufferUtil::isUtf8(kj::Array<kj::byte> buffer) {
  if (buffer.size() == 0) return true;
  return simdutf::validate_utf8(buffer.asChars().begin(), buffer.size());
}

jsg::BufferSource BufferUtil::transcode(jsg::Lock& js,
    kj::Array<kj::byte> source,
    EncodingValue rawFromEncoding,
    EncodingValue rawToEncoding) {
  auto fromEncoding = static_cast<Encoding>(rawFromEncoding);
  auto toEncoding = static_cast<Encoding>(rawToEncoding);

  JSG_REQUIRE(i18n::canBeTranscoded(fromEncoding) && i18n::canBeTranscoded(toEncoding), Error,
      "Unable to transcode buffer due to unsupported encoding");

  return i18n::transcode(js, source, fromEncoding, toEncoding);
}

}  // namespace workerd::api::node
