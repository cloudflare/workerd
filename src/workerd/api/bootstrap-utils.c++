// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap-utils.h"

#include "http.h"
#include "js-readable-stream.h"
#include "streams/readable.h"

#include <workerd/api/mimetype-js.h>
#include <workerd/jsg/url.h>

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

jsg::JsValue BootstrapUtils::mimeTypeParse(jsg::Lock& js, jsg::JsValue input) {
  auto str = JSG_REQUIRE_NONNULL(
      input.tryCast<jsg::JsString>(), TypeError, "mimeTypeParse() expects a string");
  KJ_IF_SOME(parts, parseMimeTypeToJs(js, str)) {
    return parts;
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::mimeTypeSerialize(
    jsg::Lock& js, jsg::JsValue type, jsg::JsValue subtype, jsg::JsValue params) {
  auto typeStr = JSG_REQUIRE_NONNULL(
      type.tryCast<jsg::JsString>(), TypeError, "mimeTypeSerialize() expects a string type");
  auto subtypeStr = JSG_REQUIRE_NONNULL(
      subtype.tryCast<jsg::JsString>(), TypeError, "mimeTypeSerialize() expects a string subtype");
  auto list = JSG_REQUIRE_NONNULL(
      params.tryCast<jsg::JsArray>(), TypeError, "mimeTypeSerialize() expects a parameter array");
  JSG_REQUIRE(list.size() % 2 == 0, TypeError,
      "mimeTypeSerialize() expects a name and a value for each parameter");
  return serializeMimeTypeFromJs(js, typeStr, subtypeStr, list, list.size() / 2);
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
