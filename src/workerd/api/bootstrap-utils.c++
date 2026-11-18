// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap-utils.h"

#include "http.h"
#include "js-readable-stream.h"
#include "simdutf.h"
#include "streams/readable.h"

#include <workerd/jsg/url.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

bool BootstrapUtils::isFetcher(jsg::Lock& js, jsg::JsValue value) {
  // Fetcher is registered in EW_HTTP_ISOLATE_TYPES.
  auto& handler = KJ_ASSERT_NONNULL(
      js.tryGetTypeHandler<jsg::Ref<Fetcher>>(), "Fetcher is missing from the isolate type list");
  return handler.tryUnwrap(js, value) != kj::none;
}

jsg::JsValue BootstrapUtils::parseUrl(jsg::Lock& js, jsg::JsValue input) {
  auto str =
      JSG_REQUIRE_NONNULL(input.tryCast<jsg::JsString>(), TypeError, "parseUrl() expects a string")
          .toString(js);
  KJ_IF_SOME(url, jsg::Url::tryParse(str.asPtr())) {
    auto result = js.objNoProto();
    result.set(js, "href"_kj, js.str(url.getHref()));
    result.set(js, "origin"_kj, js.str(url.getOrigin()));
    return result;
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::mimeTypeEssence(jsg::Lock& js, jsg::JsValue input) {
  auto str = JSG_REQUIRE_NONNULL(
      input.tryCast<jsg::JsString>(), TypeError, "mimeTypeEssence() expects a string")
                 .toString(js);
  KJ_IF_SOME(parsed, MimeType::tryParse(str)) {
    return js.str(parsed.essence());
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::atob(jsg::Lock& js, jsg::JsValue input) {
  auto str =
      JSG_REQUIRE_NONNULL(input.tryCast<jsg::JsString>(), TypeError, "atob() expects a string");
  v8::Local<v8::String> handle = str;
  // Three bytes for every four characters, rounded up: no less than
  // simdutf::maximal_binary_length_from_base64() gives for an input of this length.
  auto maxSize = (static_cast<size_t>(handle->Length()) + 3) / 4 * 3;
  KJ_STACK_ARRAY(kj::byte, decoded, maxSize, 1024, 1024);
  simdutf::result result;
  {
    // The string's characters in place, which nothing may allocate on the V8 heap while the
    // view lives. A code unit above 0x7F is not base64 in either representation.
    v8::String::ValueView view(js.v8Isolate, handle);
    if (view.is_one_byte()) {
      result = simdutf::base64_to_binary(reinterpret_cast<const char*>(view.data8()), view.length(),
          decoded.asChars().begin(), simdutf::base64_default);
    } else {
      result = simdutf::base64_to_binary(reinterpret_cast<const char16_t*>(view.data16()),
          view.length(), decoded.asChars().begin(), simdutf::base64_default);
    }
  }
  if (result.error != simdutf::SUCCESS) return js.undefined();
  KJ_ASSERT(result.count <= decoded.size());
  return js.str(decoded.first(result.count));
}

jsg::JsValue BootstrapUtils::btoa(jsg::Lock& js, jsg::JsValue input) {
  auto str =
      JSG_REQUIRE_NONNULL(input.tryCast<jsg::JsString>(), TypeError, "btoa() expects a string");
  if (!str.containsOnlyOneByte()) return js.undefined();
  v8::Local<v8::String> handle = str;
  auto length = static_cast<size_t>(handle->Length());
  KJ_STACK_ARRAY(kj::byte, encoded, simdutf::base64_length_from_binary(length), 1024, 1024);
  size_t written;
  {
    // As in atob(), the characters in place.
    v8::String::ValueView view(js.v8Isolate, handle);
    if (view.is_one_byte()) {
      written = simdutf::binary_to_base64(
          reinterpret_cast<const char*>(view.data8()), length, encoded.asChars().begin());
    } else {
      // A two-byte string whose code units all fit a byte.
      KJ_STACK_ARRAY(kj::byte, bytes, length, 1024, 1024);
      auto units = view.data16();
      for (auto i: kj::zeroTo(length)) {
        bytes[i] = static_cast<kj::byte>(units[i]);
      }
      written =
          simdutf::binary_to_base64(bytes.asChars().begin(), length, encoded.asChars().begin());
    }
  }
  return js.str(encoded.first(written));
}

jsg::JsValue BootstrapUtils::readableStreamState(jsg::Lock& js, jsg::JsValue value) {
  // ReadableStream is registered in EW_STREAMS_ISOLATE_TYPES whichever implementation is in use.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStream>>(),
      "ReadableStream is missing from the isolate type list");
  kj::Maybe<JsReadableStream> maybeStream;
  KJ_IF_SOME(legacy, handler.tryUnwrap(js, value)) {
    maybeStream = JsReadableStream(kj::mv(legacy));
  } else {
    maybeStream = JsReadableStream::tryUnwrapTs(js, value);
  }
  KJ_IF_SOME(stream, maybeStream) {
    if (stream.isLocked(js)) return js.str("locked"_kj);
    if (stream.isDisturbed(js)) return js.str("disturbed"_kj);
    return js.str("readable"_kj);
  }
  return js.undefined();
}

}  // namespace workerd::api
