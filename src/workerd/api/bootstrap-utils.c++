// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap-utils.h"

#include "http.h"
#include "js-readable-stream.h"
#include "streams/readable.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/url.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/stream-utils.h>

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

double BootstrapUtils::blobSizeLimit(jsg::Lock& js) {
  return static_cast<double>(Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit());
}

jsg::JsValue BootstrapUtils::blobStream(jsg::Lock& js, jsg::JsValue bytes) {
  auto u8 = JSG_REQUIRE_NONNULL(
      bytes.tryCast<jsg::JsUint8Array>(), TypeError, "blobStream() expects a Uint8Array");
  // Pass no backing so that newMemorySource copies: the bytes are a V8 ArrayBuffer's, and the
  // stream is read from the kj event loop where the isolate's MPK-protected sandbox pages are
  // unreadable.
  auto stream =
      JsReadableStream::create(js, IoContext::current(), newMemorySource(u8.asArrayPtr()));
  return kj::mv(stream).toJsValue(js);
}

void BootstrapUtils::recordBlobUse(jsg::Lock& js, jsg::JsValue kind) {
  auto n = JSG_REQUIRE_NONNULL(
      kind.tryCast<jsg::JsNumber>(), TypeError, "recordBlobUse() expects a number");
  switch (static_cast<int>(KJ_REQUIRE_NONNULL(n.value(js)))) {
    case 0:
      FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_ARRAY_BUFFER);
      break;
    case 1:
      FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_TEXT);
      break;
    case 2:
      FeatureObserver::maybeRecordUse(FeatureObserver::Feature::BLOB_AS_STREAM);
      break;
    default:
      JSG_FAIL_REQUIRE(TypeError, "recordBlobUse() expects 0, 1 or 2");
  }
}

}  // namespace workerd::api
