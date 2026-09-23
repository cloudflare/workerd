// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "buffer-native.h"

#include "buffer-string-search.h"
#include "i18n.h"

#include <nbytes.h>
#include <simdutf.h>

#include <kj/encoding.h>

#include <cstring>

namespace workerd::api::node {

namespace {

// Only these flags are accepted. Every write passes V8 the capacity of an exact
// range, so a null terminator would be written past it.
jsg::JsString::WriteFlags toWriteFlags(uint8_t flags) {
  JSG_REQUIRE(flags == jsg::JsString::WriteFlags::NONE ||
          flags == jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8,
      TypeError, "Invalid write flags");
  return static_cast<jsg::JsString::WriteFlags>(flags);
}

// The `[start, end)` slice of `array`.
kj::ArrayPtr<kj::byte> range(jsg::JsUint8Array& array, double start, double end) {
  JSG_REQUIRE(
      0 <= start && start <= end && end <= array.size(), RangeError, "Range is out of bounds");
  return array.asArrayPtr().slice(static_cast<size_t>(start), static_cast<size_t>(end));
}

// Checks the arguments that SearchString relies on the caller for: a non-empty
// needle that fits in the haystack and, for a forward search, a start index
// that leaves room for the needle.
void requireSearchArguments(
    size_t haystackLength, size_t needleLength, double startIndex, bool isForward) {
  JSG_REQUIRE(needleLength > 0 && needleLength <= haystackLength && startIndex >= 0 &&
          (!isForward || startIndex <= haystackLength - needleLength),
      RangeError, "Invalid search arguments");
}

}  // namespace

// jsg::JsString::utf8Length()
double BufferNative::utf8Length(jsg::Lock& js, jsg::JsString string) {
  return string.utf8Length(js);
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<kj::byte>, flags): writes the low
// byte of each UTF-16 code unit. Returns the number of bytes written.
double BufferNative::writeOneByte(jsg::Lock& js,
    jsg::JsString string,
    jsg::JsUint8Array dest,
    double start,
    double end,
    uint8_t flags) {
  return string.writeInto(js, range(dest, start, end), toWriteFlags(flags)).written;
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<char>, flags): writes UTF-8.
// Returns the number of bytes written.
double BufferNative::writeUtf8(jsg::Lock& js,
    jsg::JsString string,
    jsg::JsUint8Array dest,
    double start,
    double end,
    uint8_t flags) {
  return string.writeInto(js, range(dest, start, end).asChars(), toWriteFlags(flags)).written;
}

// jsg::JsString::writeInto(js, kj::ArrayPtr<uint16_t>, flags) over the range
// reinterpreted as native-endian uint16_ts (the last byte of an odd-length
// range is not written). Returns the number of code units written.
double BufferNative::writeUtf16(jsg::Lock& js,
    jsg::JsString string,
    jsg::JsUint8Array dest,
    double start,
    double end,
    uint8_t flags) {
  auto options = toWriteFlags(flags);
  auto bytes = range(dest, start, end);
#if __has_feature(undefined_behavior_sanitizer)
  // UBSan warns about unaligned writes, which are hard to avoid if dest is
  // unaligned. Write through an aligned buffer instead, as in buffer.c++.
  auto tmpBuf = kj::heapArray<uint16_t>(bytes.size() / sizeof(uint16_t));
  auto result = string.writeInto(js, tmpBuf, options);
  kj::ArrayPtr<uint16_t> buf(reinterpret_cast<uint16_t*>(bytes.begin()), result.written);
  buf.copyFrom(tmpBuf.first(result.written));
#else
  kj::ArrayPtr<uint16_t> buf(
      reinterpret_cast<uint16_t*>(bytes.begin()), bytes.size() / sizeof(uint16_t));
  auto result = string.writeInto(js, buf, options);
#endif
  return result.written;
}

// jsg::Lock::str(kj::ArrayPtr<const kj::byte>)
jsg::JsString BufferNative::newFromOneByte(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  return js.str(range(bytes, start, end).asConst());
}

// jsg::Lock::str(kj::ArrayPtr<const char>)
jsg::JsString BufferNative::newFromUtf8(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  return js.str(range(bytes, start, end).asChars().asConst());
}

// jsg::Lock::str(kj::ArrayPtr<const uint16_t>) over the range reinterpreted as
// native-endian uint16_ts (a trailing odd byte is ignored). Copies to an
// aligned buffer first, as BufferUtil's toString does.
jsg::JsString BufferNative::newFromTwoByte(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  auto slice = range(bytes, start, end);
  kj::ArrayPtr<uint16_t> view(reinterpret_cast<uint16_t*>(slice.begin()), slice.size() / 2);
  KJ_STACK_ARRAY(uint16_t, data, view.size(), 1024, 4096);
  data.copyFrom(view);
  return js.str(data.asConst());
}

// SearchString over one-byte characters, as in BufferUtil's indexOf for
// encodings other than UTF-16. Returns the position of the match, or the
// haystack length if there is none.
double BufferNative::searchOneByte(
    jsg::JsUint8Array haystack, jsg::JsUint8Array needle, double startIndex, bool isForward) {
  auto hayStack = haystack.asArrayPtr();
  auto pattern = needle.asArrayPtr();
  requireSearchArguments(hayStack.size(), pattern.size(), startIndex, isForward);
  return SearchString(hayStack.begin(), hayStack.size(), pattern.begin(), pattern.size(),
      static_cast<size_t>(startIndex), isForward);
}

// SearchString over the first `haystackLength` and `needleLength` UTF-16 code
// units of `haystack` and `needle`, as in BufferUtil's indexOf for UTF-16.
// BufferUtil copies both into aligned kj::SmallArrays; here an array that is
// already aligned is searched in place. Returns the position of the match, in
// code units, or `haystackLength` if there is none.
double BufferNative::searchTwoByte(jsg::JsUint8Array haystack,
    double haystackLength,
    jsg::JsUint8Array needle,
    double needleLength,
    double startIndex,
    bool isForward) {
  auto hayStackBytes = range(haystack, 0, haystackLength * 2);
  auto needleBytes = range(needle, 0, needleLength * 2);
  auto hayStackU16Len = hayStackBytes.size() / 2;
  auto needleU16Len = needleBytes.size() / 2;
  requireSearchArguments(hayStackU16Len, needleU16Len, startIndex, isForward);

  auto isAligned = [](kj::ArrayPtr<kj::byte> bytes) {
    return reinterpret_cast<uintptr_t>(bytes.begin()) % alignof(uint16_t) == 0;
  };
  kj::SmallArray<uint16_t, 1024> alignedHayStack(isAligned(hayStackBytes) ? 0 : hayStackU16Len);
  const uint16_t* hayStackChars;
  if (isAligned(hayStackBytes)) {
    hayStackChars = reinterpret_cast<const uint16_t*>(hayStackBytes.begin());
  } else {
    alignedHayStack.asBytes().copyFrom(hayStackBytes);
    hayStackChars = alignedHayStack.begin();
  }
  kj::SmallArray<uint16_t, 1024> alignedNeedle(isAligned(needleBytes) ? 0 : needleU16Len);
  const uint16_t* needleChars;
  if (isAligned(needleBytes)) {
    needleChars = reinterpret_cast<const uint16_t*>(needleBytes.begin());
  } else {
    alignedNeedle.asBytes().copyFrom(needleBytes);
    needleChars = alignedNeedle.begin();
  }

  return SearchString(hayStackChars, hayStackU16Len, needleChars, needleU16Len,
      static_cast<size_t>(startIndex), isForward);
}

// memcmp over `length` bytes of each array, from the given starts.
int BufferNative::memcmp(
    jsg::JsUint8Array one, double oneStart, jsg::JsUint8Array two, double twoStart, double length) {
  auto ptrOne = range(one, oneStart, oneStart + length);
  auto ptrTwo = range(two, twoStart, twoStart + length);
  return ptrOne.size() > 0 ? ::memcmp(ptrOne.begin(), ptrTwo.begin(), ptrOne.size()) : 0;
}

// The BASE64 and BASE64URL cases of decodeStringImpl in buffer.c++: writes the
// string's low bytes into a stack buffer, then decodes it with
// simdutf::base64_to_binary and base64_default_or_url_accept_garbage, ignoring
// the error code.
jsg::JsUint8Array BufferNative::decodeBase64(jsg::Lock& js, jsg::JsString string) {
  auto length = string.length(js);
  // We do not use the kj::String conversion here because inline null-characters
  // need to be ignored.
  KJ_STACK_ARRAY(kj::byte, buf, length, 1024, 536870888);
  auto result = string.writeInto(js, buf, jsg::JsString::REPLACE_INVALID_UTF8);
  auto len = result.written;
  auto dest = jsg::JsUint8Array::create(
      js, simdutf::maximal_binary_length_from_base64(buf.asChars().begin(), len));
  auto decResult = simdutf::base64_to_binary(buf.asChars().begin(), len,
      dest.asArrayPtr().asChars().begin(), simdutf::base64_default_or_url_accept_garbage);
  if (decResult.count < dest.size()) {
    return dest.slice(js, decResult.count);
  }
  return dest;
}

// The BASE64 case of toStringImpl in buffer.c++: simdutf::binary_to_base64
// with base64_default into a stack buffer.
jsg::JsString BufferNative::encodeBase64(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  auto slice = range(bytes, start, end);
  size_t length = simdutf::base64_length_from_binary(slice.size());
  KJ_STACK_ARRAY(kj::byte, out, length, 1024, 4096);
  simdutf::binary_to_base64(reinterpret_cast<const char*>(slice.begin()), slice.size(),
      reinterpret_cast<char*>(out.begin()));
  return js.str(out.asConst());
}

// The BASE64URL case of toStringImpl in buffer.c++: simdutf::binary_to_base64
// with base64_url into a stack buffer.
jsg::JsString BufferNative::encodeBase64Url(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  auto slice = range(bytes, start, end);
  auto options = simdutf::base64_url;
  size_t length = simdutf::base64_length_from_binary(slice.size(), options);
  KJ_STACK_ARRAY(kj::byte, out, length, 1024, 4096);
  simdutf::binary_to_base64(reinterpret_cast<const char*>(slice.begin()), slice.size(),
      reinterpret_cast<char*>(out.begin()), options);
  return js.str(out.asConst());
}

// simdutf::validate_ascii
bool BufferNative::simdutfValidateAscii(jsg::JsUint8Array input) {
  return simdutf::validate_ascii(input.asArrayPtr().asChars().begin(), input.size());
}

// simdutf::validate_utf8
bool BufferNative::simdutfValidateUtf8(jsg::JsUint8Array input) {
  return simdutf::validate_utf8(input.asArrayPtr().asChars().begin(), input.size());
}

// nbytes::Base64Decode into the `[start, end)` range of `output`. Returns the
// number of bytes written, which is at most the size of the range.
double BufferNative::nbytesBase64Decode(
    jsg::JsUint8Array output, double start, double end, jsg::JsUint8Array input) {
  auto dest = range(output, start, end);
  return nbytes::Base64Decode(
      dest.asChars().begin(), dest.size(), input.asArrayPtr().asChars().begin(), input.size());
}

// nbytes::SwapBytes16
bool BufferNative::nbytesSwapBytes16(jsg::JsUint8Array data) {
  return nbytes::SwapBytes16(data.asArrayPtr().asChars().begin(), data.size());
}

// nbytes::SwapBytes32
bool BufferNative::nbytesSwapBytes32(jsg::JsUint8Array data) {
  return nbytes::SwapBytes32(data.asArrayPtr().asChars().begin(), data.size());
}

// nbytes::SwapBytes64
bool BufferNative::nbytesSwapBytes64(jsg::JsUint8Array data) {
  return nbytes::SwapBytes64(data.asArrayPtr().asChars().begin(), data.size());
}

// The HEX case of toStringImpl in buffer.c++: kj::encodeHex.
jsg::JsString BufferNative::encodeHex(
    jsg::Lock& js, jsg::JsUint8Array bytes, double start, double end) {
  return js.str(kj::encodeHex(range(bytes, start, end)));
}

// i18n::transcode
jsg::JsUint8Array BufferNative::transcode(
    jsg::Lock& js, jsg::JsUint8Array source, uint8_t fromEncoding, uint8_t toEncoding) {
  auto from = static_cast<Encoding>(fromEncoding);
  auto to = static_cast<Encoding>(toEncoding);
  JSG_REQUIRE(i18n::canBeTranscoded(from) && i18n::canBeTranscoded(to), Error,
      "Unable to transcode buffer due to unsupported encoding");
  return i18n::transcode(js, source.asArrayPtr(), from, to);
}

}  // namespace workerd::api::node
