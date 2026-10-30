// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class AbortSignal;
class Fetcher;
class Response;

// The I/O of an EventSource implemented in the per-isolate bootstrap (src/per_isolate/), bound to
// the request that created it. Obtained only from the bootstrap's utils.createEventSourceIo(); the
// bootstrap never lets one reach user code. Every method but isCurrent() throws, as
// IoContext::requireCurrentOrThrowJs() does, unless called within the request it is bound to.
class EventSourceIo final: public jsg::Object {
 public:
  explicit EventSourceIo(IoContext::Id contextId): contextId(contextId) {}

  // Whether this is running within the request this is bound to.
  bool isCurrent();

  // Throws unless running within the request this is bound to.
  void requireCurrent();

  // Fetches `url` through `fetcher`, or as the global fetch() does when there is none, with the
  // request headers of an EventSource connection: `Accept: text/event-stream`,
  // `Cache-Control: no-cache` and, when `lastEventId` is not empty, `Last-Event-ID`.
  jsg::Promise<jsg::Ref<Response>> fetch(jsg::Lock& js,
      kj::Maybe<jsg::Ref<Fetcher>> fetcher,
      kj::String url,
      kj::String lastEventId,
      jsg::Ref<AbortSignal> signal);

  // Resolves after `milliseconds` on the request's after-limit timer, or rejects with `signal`'s
  // reason when it aborts first.
  jsg::Promise<void> wait(jsg::Lock& js, uint32_t milliseconds, jsg::Ref<AbortSignal> signal);

  // Logs `error` as an exception uncaught by an asynchronous task of the request.
  void logUncaughtException(jsg::Lock& js, jsg::JsValue error);

  JSG_RESOURCE_TYPE(EventSourceIo) {
    JSG_METHOD(isCurrent);
    JSG_METHOD(requireCurrent);
    JSG_METHOD(fetch);
    JSG_METHOD(wait);
    JSG_METHOD(logUncaughtException);
    JSG_TS_OVERRIDE(type EventSourceIo = never);
  }

 private:
  IoContext::Id contextId;
};

// The bootstrap's EventSource helpers, exposed as utils members. They are stateless and run no
// user code.
class EventSourceBootstrap {
 public:
  // An EventSourceIo bound to the current request, or undefined outside of one.
  static jsg::JsValue createEventSourceIo(jsg::Lock& js);

  // Whether `value` is a Fetcher, which an EventSourceInit's `fetcher` must be.
  static bool isFetcher(jsg::Lock& js, jsg::JsValue value);

  // Parses `input` (a string) as an absolute URL, as the C++ EventSource does, without a base
  // and whichever URL implementation the compat flags give the global. Returns a null-prototype
  // record of its serialization (`href`) and its origin's (`origin`), or undefined if it is not a
  // valid URL. Throws a TypeError if `input` is not a string.
  static jsg::JsValue parseUrl(jsg::Lock& js, jsg::JsValue input);

  // Whether `input` (a string) parses as a MIME type whose essence is text/event-stream. Throws a
  // TypeError if `input` is not a string.
  static bool isEventStreamMimeType(jsg::Lock& js, jsg::JsValue input);

  // Undefined if `value` is not a ReadableStream of either implementation, else "locked",
  // "disturbed" (unlocked, but read from) or "readable".
  static jsg::JsValue readableStreamState(jsg::Lock& js, jsg::JsValue value);
};

}  // namespace workerd::api
