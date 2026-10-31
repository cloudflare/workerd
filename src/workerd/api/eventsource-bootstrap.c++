// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "eventsource-bootstrap.h"

#include "basics.h"
#include "http.h"
#include "js-readable-stream.h"
#include "streams/readable.h"

#include <workerd/jsg/url.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

bool EventSourceIo::isCurrent() {
  return contextId.isCurrent();
}

void EventSourceIo::requireCurrent() {
  IoContext::requireCurrentOrThrowJs(contextId);
}

jsg::Promise<jsg::Ref<Response>> EventSourceIo::fetch(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    kj::String url,
    kj::String lastEventId,
    jsg::Ref<AbortSignal> signal) {
  requireCurrent();
  auto headers = js.alloc<Headers>();
  headers->setCommon(capnp::CommonHeaderName::ACCEPT, MimeType::EVENT_STREAM.essence());
  headers->setCommon(capnp::CommonHeaderName::CACHE_CONTROL, kj::str("no-cache"));
  if (lastEventId != ""_kj) {
    headers->setUnguarded(js, kj::str("last-event-id"), kj::mv(lastEventId));
  }
  return fetchImpl(js, kj::mv(fetcher), kj::mv(url),
      RequestInitializerDict{
        .headers = kj::mv(headers),
        .signal = kj::mv(signal),
      });
}

jsg::Promise<void> EventSourceIo::wait(
    jsg::Lock& js, uint32_t milliseconds, jsg::Ref<AbortSignal> signal) {
  requireCurrent();
  auto& context = IoContext::current();
  return context.awaitIo(
      js, signal->wrap(js, context.afterLimitTimeout(milliseconds * kj::MILLISECONDS)));
}

void EventSourceIo::logUncaughtException(jsg::Lock& js, jsg::JsValue error) {
  requireCurrent();
  IoContext::current().logUncaughtException(UncaughtExceptionSource::ASYNC_TASK, error);
}

jsg::JsValue EventSourceBootstrap::createEventSourceIo(jsg::Lock& js) {
  if (!IoContext::hasCurrent()) return js.undefined();
  // EventSourceIo is registered in EW_EVENTSOURCE_ISOLATE_TYPES.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<EventSourceIo>>(),
      "EventSourceIo is missing from the isolate type list");
  return jsg::JsValue(handler.wrap(js, js.alloc<EventSourceIo>(IoContext::current().getId())));
}

bool EventSourceBootstrap::isFetcher(jsg::Lock& js, jsg::JsValue value) {
  // Fetcher is registered in EW_HTTP_ISOLATE_TYPES.
  auto& handler = KJ_ASSERT_NONNULL(
      js.tryGetTypeHandler<jsg::Ref<Fetcher>>(), "Fetcher is missing from the isolate type list");
  return handler.tryUnwrap(js, value) != kj::none;
}

jsg::JsValue EventSourceBootstrap::parseUrl(jsg::Lock& js, jsg::JsValue input) {
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

bool EventSourceBootstrap::isEventStreamMimeType(jsg::Lock& js, jsg::JsValue input) {
  auto str = JSG_REQUIRE_NONNULL(
      input.tryCast<jsg::JsString>(), TypeError, "isEventStreamMimeType() expects a string")
                 .toString(js);
  KJ_IF_SOME(parsed, MimeType::tryParse(str)) {
    return MimeType::EVENT_STREAM == parsed;
  }
  return false;
}

jsg::JsValue EventSourceBootstrap::readableStreamState(jsg::Lock& js, jsg::JsValue value) {
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
